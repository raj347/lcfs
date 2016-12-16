#include "includes.h"

/* XXX Check return values from fuse_reply_*, for example on interrupt */
/* XXX Do we need to track lookup counts and forget? */

/* Initialize default values in fuse_entry_param structure.
 */
void
lc_epInit(struct fuse_entry_param *ep) {
    assert(ep->ino > LC_ROOT_INODE);
    ep->attr.st_ino = ep->ino;
    ep->generation = 1;
    ep->attr_timeout = 0;
    ep->entry_timeout = 1.0;
}

/* Copy disk inode to stat structure */
void
lc_copyStat(struct stat *st, struct inode *inode) {
    struct dinode *dinode = &inode->i_dinode;

    st->st_dev = 0;
    st->st_ino = dinode->di_ino;
    st->st_mode = dinode->di_mode;
    st->st_nlink = dinode->di_nlink;
    st->st_uid = dinode->di_uid;
    st->st_gid = dinode->di_gid;
    st->st_rdev = dinode->di_rdev;
    st->st_size = dinode->di_size;
    st->st_blksize = LC_BLOCK_SIZE;
    st->st_blocks = dinode->di_blocks;
    st->st_atim = dinode->di_mtime;
    st->st_mtim = dinode->di_mtime;
    st->st_ctim = dinode->di_ctime;
}

/* Create a new directory entry and associated inode */
static int
create(struct fs *fs, ino_t parent, const char *name, mode_t mode,
       uid_t uid, gid_t gid, dev_t rdev, const char *target,
       struct fuse_file_info *fi, struct fuse_entry_param *ep) {
    struct gfs *gfs = fs->fs_gfs;
    struct inode *dir, *inode;
    ino_t ino;

    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, parent, EROFS);
        return EROFS;
    }
    if (parent == gfs->gfs_snap_root) {
        lc_reportError(__func__, __LINE__, parent, EPERM);
        return EPERM;
    }
    if (!lc_hasSpace(gfs, fs->fs_pcount + 1)) {
        lc_reportError(__func__, __LINE__, parent, ENOSPC);
        return ENOSPC;
    }
    dir = lc_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_mode));
    if (dir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(dir);
    }
    inode = lc_inodeInit(fs, mode, uid, gid, rdev, parent, target);
    ino = inode->i_ino;
    lc_dirAdd(dir, ino, mode, name, strlen(name));
    if (S_ISDIR(mode)) {
        assert(inode->i_nlink >= 2);
        assert(dir->i_nlink >= 2);
        dir->i_nlink++;
    }
    lc_updateInodeTimes(dir, true, true);
    lc_copyStat(&ep->attr, inode);
    if (fi) {
        inode->i_ocount++;
        fi->fh = (uint64_t)inode;
    }
    if ((dir->i_flags & LC_INODE_TMP) ||
        (dir->i_ino == gfs->gfs_tmp_root)) {
        inode->i_flags |= LC_INODE_TMP;
    }
    lc_markInodeDirty(inode, true, false, false, false);
    lc_markInodeDirty(dir, true, true, false, false);
    lc_inodeUnlock(inode);
    lc_inodeUnlock(dir);
    ep->ino = lc_setHandle(fs->fs_gindex, ino);
    lc_epInit(ep);
    return 0;
}

/* Truncate a file */
static void
lc_truncate(struct inode *inode, off_t size) {
    assert(S_ISREG(inode->i_mode));

    if (size < inode->i_size) {
        lc_truncPages(inode, size, true);
    }
    assert(!(inode->i_flags & LC_INODE_SHARED));
    inode->i_size = size;
}

/* Remove an inode */
int
lc_removeInode(struct fs *fs, struct inode *dir, ino_t ino, bool rmdir,
               void **inodep) {
    bool removed = false, unlock = true;
    struct inode *inode;

    assert(S_ISDIR(dir->i_mode));
    inode = lc_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ESTALE);
        return ESTALE;
    }
    assert(inode->i_nlink);
    if (rmdir) {
        assert(inode->i_parent == dir->i_ino);

        /* Allow directory removals from the root file system even when
         * directories are not empty.
         */
        if (inode->i_size && (fs == lc_getGlobalFs(fs->fs_gfs))) {
            lc_removeTree(fs, inode);
        }
        if (inode->i_size) {
            lc_inodeUnlock(inode);
            //lc_reportError(__func__, __LINE__, ino, EEXIST);
            return EEXIST;
        }
        assert(inode->i_size == 0);
        assert(inode->i_nlink == 2);
        inode->i_nlink = 0;
        if (inode->i_flags & LC_INODE_DHASHED) {
            lc_dirFreeHash(fs, inode);
        }
        inode->i_flags |= LC_INODE_REMOVED;
        removed = true;
    } else {
        inode->i_nlink--;

        /* Flag a file as removed on last unlink */
        if (inode->i_nlink == 0) {
            if ((inode->i_ocount == 0) && S_ISREG(inode->i_mode)) {
                if (inodep) {
                    *inodep = inode;
                    unlock = false;
                } else {
                    lc_truncate(inode, 0);
                }
            }
            inode->i_flags |= LC_INODE_REMOVED;
            removed = true;
        }
    }
    lc_markInodeDirty(inode, true, rmdir, S_ISREG(inode->i_mode),
                      false);
    if (unlock) {
        lc_inodeUnlock(inode);
    }
    if (removed) {
        __sync_sub_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, 1);
        lc_updateFtypeStats(fs, inode->i_mode, false);
    }
    return 0;
}

/* Remove a directory entry */
static int
lc_remove(struct fs *fs, ino_t parent, const char *name, void **inodep,
          bool rmdir) {
    struct inode *dir;
    int err;

    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, parent, EROFS);
        return EROFS;
    }
    dir = lc_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_mode));
    if (dir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(dir);
    }
    err = lc_dirRemoveName(fs, dir, name, rmdir, inodep, lc_removeInode);
    lc_inodeUnlock(dir);
    if (err) {
        if (err != EEXIST) {
            lc_reportError(__func__, __LINE__, parent, err);
        }
    }
    return err;
}

/* Lookup the specified name in the specified directory */
static void
lc_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct fuse_entry_param ep;
    struct inode *inode, *dir;
    struct fs *fs, *nfs = NULL;
    struct timeval start;
    int gindex, err = 0;
    ino_t ino;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getfs(parent, false);
    dir = lc_getInode(fs, parent, NULL, false, false);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    ino = lc_dirLookup(fs, dir, name);
    if (ino == LC_INVALID_INODE) {
        lc_inodeUnlock(dir);

        /* Let kernel remember lookup failure as a negative entry */
        memset(&ep, 0, sizeof(struct fuse_entry_param));
        ep.entry_timeout = 1.0;
        fuse_reply_entry(req, &ep);
        err = ENOENT;
        goto out;
    }

    /* Check if looking up a snapshot root */
    if (parent == fs->fs_gfs->gfs_snap_root) {
        gindex = lc_getIndex(fs, parent, ino);
        if (fs->fs_gindex != gindex) {
            nfs = lc_getfs(lc_setHandle(gindex, ino), false);
        }
    } else {
        gindex = fs->fs_gindex;
    }
    inode = lc_getInode(nfs ? nfs : fs, ino, NULL, false, false);
    lc_inodeUnlock(dir);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
    } else {
        lc_copyStat(&ep.attr, inode);
        lc_inodeUnlock(inode);
        ep.ino = lc_setHandle(gindex, ino);
        lc_epInit(&ep);
        fuse_reply_entry(req, &ep);
    }

out:
    lc_statsAdd(nfs ? nfs : fs, LC_LOOKUP, err, &start);
    lc_unlock(fs);
    if (nfs) {
        lc_unlock(nfs);
    }
}

/* Get attributes of a file */
static void
lc_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct inode *inode;
    struct stat stbuf;
    struct fs *fs;
    ino_t parent;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, 0, ino, NULL);
    fs = lc_getfs(ino, false);
    inode = lc_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    lc_copyStat(&stbuf, inode);
    parent = inode->i_parent;
    lc_inodeUnlock(inode);
    stbuf.st_ino = lc_setHandle(lc_getIndex(fs, parent,
                                              stbuf.st_ino), stbuf.st_ino);
    fuse_reply_attr(req, &stbuf, 1.0);

out:
    lc_statsAdd(fs, LC_GETATTR, err, &start);
    lc_unlock(fs);
}

/* Change the attributes of the specified inode as requested */
static void
lc_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
            int to_set, struct fuse_file_info *fi) {
    bool ctime = false, mtime = false, flush = false;
    struct inode *inode, *handle;
    struct timeval start;
    struct stat stbuf;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    fs = lc_getfs(ino, false);
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    handle = fi ? (struct inode *)fi->fh : NULL;
    inode = lc_getInode(fs, ino, handle, true, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    if (to_set & FUSE_SET_ATTR_MODE) {
        assert((inode->i_mode & S_IFMT) == (attr->st_mode & S_IFMT));
        inode->i_mode = attr->st_mode;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_UID) {
        inode->i_dinode.di_uid = attr->st_uid;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_GID) {
        inode->i_dinode.di_gid = attr->st_gid;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_SIZE) {
        flush = (attr->st_size < inode->i_size) &&
                inode->i_private && inode->i_dinode.di_blocks;
        lc_truncate(inode, attr->st_size);
        mtime = true;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
        inode->i_dinode.di_mtime = attr->st_mtim;
        mtime = false;
    } else if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
        mtime = true;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_CTIME) {
        inode->i_dinode.di_ctime = attr->st_ctim;
        ctime = false;
    }
    if (ctime || mtime) {
        lc_updateInodeTimes(inode, mtime, ctime);
    }
    lc_markInodeDirty(inode, true, false, to_set & FUSE_SET_ATTR_SIZE, false);
    lc_copyStat(&stbuf, inode);
    lc_inodeUnlock(inode);
    stbuf.st_ino = lc_setHandle(fs->fs_gindex, stbuf.st_ino);
    fuse_reply_attr(req, &stbuf, 1.0);

out:
    lc_statsAdd(fs, LC_SETATTR, err, &start);
    if (flush && fs->fs_fdextents) {
        lc_flushDirtyPages(fs->fs_gfs, fs);
    }
    lc_unlock(fs);
}

/* Read target information for a symbolic link */
static void
lc_readlink(fuse_req_t req, fuse_ino_t ino) {
    char buf[LC_FILENAME_MAX + 1];
    struct timeval start;
    struct inode *inode;
    int size, err = 0;
    struct fs *fs;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, 0, ino, NULL);
    fs = lc_getfs(ino, false);
    inode = lc_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISLNK(inode->i_mode));
    size = inode->i_size;
    assert(size && (size <= LC_FILENAME_MAX));
    strncpy(buf, inode->i_target, size);
    lc_inodeUnlock(inode);
    buf[size] = 0;
    fuse_reply_readlink(req, buf);

out:
    lc_statsAdd(fs, LC_READLINK, err, &start);
    lc_unlock(fs);
}

/* Create a special file */
static void
lc_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
          mode_t mode, dev_t rdev) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getfs(parent, false);
    err = create(fs, parent, name, mode & ~ctx->umask,
                 ctx->uid, ctx->gid, rdev, NULL, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
    lc_statsAdd(fs, LC_MKNOD, err, &start);
    lc_unlock(fs);
}

/* Create a directory */
static void
lc_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct gfs *gfs;
    struct fs *fs;
    bool global;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getfs(parent, false);
    err = create(fs, parent, name, S_IFDIR | (mode & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, NULL, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);

        /* Remember some special directories created */
        gfs = getfs();
        global = lc_getInodeHandle(parent) == LC_ROOT_INODE;
        if (global && (strcmp(name, "lcfs") == 0)) {
            lc_setSnapshotRoot(gfs, e.ino);
        } else if (global && (strcmp(name, "tmp") == 0)) {
            gfs->gfs_tmp_root = e.ino;
            printf("tmp root %ld\n", e.ino);
        }
    }
    lc_statsAdd(fs, LC_MKDIR, err, &start);
    lc_unlock(fs);
}

/* Remove a file */
static void
lc_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct inode *inode = NULL;
    struct timeval start;
    bool flush = false;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getfs(parent, false);
    err = lc_remove(fs, parent, name, (void **)&inode, false);
    fuse_reply_err(req, err);
    if (inode) {
        flush = inode->i_private && inode->i_dinode.di_blocks;
        lc_truncate(inode, 0);
        lc_inodeUnlock(inode);
    }
    lc_statsAdd(fs, LC_UNLINK, err, &start);
    if (flush && fs->fs_fdextents) {
        lc_flushDirtyPages(fs->fs_gfs, fs);
    }
    lc_unlock(fs);
}

/* Remove a special directory */
static void
lc_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getfs(parent, false);
    err = lc_remove(fs, parent, name, NULL, true);
    fuse_reply_err(req, err);
    lc_statsAdd(fs, LC_RMDIR, err, &start);
    lc_unlock(fs);
}

/* Create a symbolic link */
static void
lc_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
            const char *name) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getfs(parent, false);
    err = create(fs, parent, name, S_IFLNK | (0777 & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, link, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
    lc_statsAdd(fs, LC_SYMLINK, err, &start);
    lc_unlock(fs);
}

/* Rename a file to another (mv) */
static void
lc_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
          fuse_ino_t newparent, const char *newname, unsigned int flags) {
    struct inode *inode, *sdir, *tdir = NULL;
    struct timeval start;
    struct fs *fs;
    int err = 0;
    ino_t ino;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, newparent, name);
    fs = lc_getfs(parent, false);
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, parent, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }

    /* Follow some locking order while locking the directories */
    if (parent > newparent) {
        tdir = lc_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            lc_reportError(__func__, __LINE__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        assert(S_ISDIR(tdir->i_mode));
    }
    sdir = lc_getInode(fs, parent, NULL, true, true);
    if (sdir == NULL) {
        if (tdir) {
            lc_inodeUnlock(tdir);
        }
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISDIR(sdir->i_mode));
    ino = lc_dirLookup(fs, sdir, name);
    if (ino == LC_INVALID_INODE) {
        lc_inodeUnlock(sdir);
        if (tdir) {
            lc_inodeUnlock(tdir);
        }
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    if (sdir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(sdir);
    }
    if (parent < newparent) {
        tdir = lc_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            lc_inodeUnlock(sdir);
            lc_reportError(__func__, __LINE__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        assert(S_ISDIR(tdir->i_mode));
    }
    if (tdir && (tdir->i_flags & LC_INODE_SHARED)) {
        lc_dirCopy(tdir);
    }
    lc_dirRemoveName(fs, tdir ? tdir : sdir, newname, false,
                     NULL, lc_removeInode);

    /* Renaming to another directory */
    if (parent != newparent) {
        inode = lc_getInode(fs, ino, NULL, true, true);
        if (inode == NULL) {
            lc_inodeUnlock(sdir);
            lc_inodeUnlock(tdir);
            lc_reportError(__func__, __LINE__, ino, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        lc_dirAdd(tdir, ino, inode->i_mode, newname, strlen(newname));
        lc_dirRemove(sdir, name);
        if (S_ISDIR(inode->i_mode)) {
            assert(sdir->i_nlink > 2);
            sdir->i_nlink--;
            assert(tdir->i_nlink >= 2);
            tdir->i_nlink++;
        }
        inode->i_parent = lc_getInodeHandle(newparent);
        lc_updateInodeTimes(inode, false, true);
        lc_markInodeDirty(inode, true, false, false, false);
        lc_inodeUnlock(inode);
    } else {

        /* Rename within the directory */
        lc_dirRename(sdir, ino, name, newname);
    }
    fuse_reply_err(req, 0);
    lc_updateInodeTimes(sdir, true, true);
    lc_markInodeDirty(sdir, true, true, false, false);
    lc_inodeUnlock(sdir);
    if (tdir) {
        lc_updateInodeTimes(tdir, true, true);
        lc_markInodeDirty(tdir, true, true, false, false);
        lc_inodeUnlock(tdir);
    }

out:
    lc_statsAdd(fs, LC_RENAME, err, &start);
    lc_unlock(fs);
}

/* Create a new link to an inode */
static void
lc_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
         const char *newname) {
    struct fuse_entry_param ep;
    struct inode *inode, *dir;
    struct timeval start;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, newparent, ino, newname);
    fs = lc_getfs(ino, false);
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    dir = lc_getInode(fs, newparent, NULL, true, true);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, newparent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISDIR(dir->i_mode));
    assert(dir->i_nlink >= 2);
    if (dir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(dir);
    }
    inode = lc_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        lc_inodeUnlock(dir);
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(!S_ISDIR(inode->i_mode));
    lc_dirAdd(dir, inode->i_ino, inode->i_mode, newname,
               strlen(newname));
    lc_updateInodeTimes(dir, true, true);
    lc_markInodeDirty(dir, true, true, false, false);
    inode->i_nlink++;
    lc_updateInodeTimes(inode, false, true);
    lc_markInodeDirty(inode, true, false, false, false);
    lc_inodeUnlock(dir);
    lc_copyStat(&ep.attr, inode);
    lc_inodeUnlock(inode);
    ep.ino = lc_setHandle(fs->fs_gindex, ino);
    lc_epInit(&ep);
    fuse_reply_entry(req, &ep);

out:
    lc_statsAdd(fs, LC_LINK, err, &start);
    lc_unlock(fs);
}

/* Set up file handle in case file is shared from another file system */
static int
lc_openInode(struct fs *fs, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct inode *inode;
    bool modify;

    fi->fh = 0;
    modify = (fi->flags & (O_WRONLY | O_RDWR));
    if (modify && fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        return EROFS;
    }
    inode = lc_getInode(fs, ino, NULL, modify, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        return ENOENT;
    }

    /* Do not allow opening a removed inode */
    if (inode->i_flags & LC_INODE_REMOVED) {
        lc_inodeUnlock(inode);
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        return ENOENT;
    }

    /* Increment open count if inode is private to this layer.
     */
    if (inode->i_fs == fs) {
        fi->keep_cache = inode->i_private;
        inode->i_ocount++;
    }
    fi->fh = (uint64_t)inode;
    lc_inodeUnlock(inode);
    return 0;
}

/* Open a file and return a handle corresponding to the inode number */
static void
lc_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, 0, ino, NULL);
    fs = lc_getfs(ino, false);
    err = lc_openInode(fs, ino, fi);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        /* XXX explore returning ENOSYS with FUSE_CAP_NO_OPEN_SUPPORT */
        fuse_reply_open(req, fi);
    }
    lc_statsAdd(fs, LC_OPEN, err, &start);
    lc_unlock(fs);
}

/* Read from a file */
static void
lc_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
        struct fuse_file_info *fi) {
    struct fuse_bufvec *bufv;
    struct timeval start;
    struct inode *inode;
    struct page **pages;
    off_t endoffset;
    uint64_t pcount;
    struct fs *fs;
    size_t fsize;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    if (size == 0) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }
    pcount = (size / LC_BLOCK_SIZE) + 2;
    fsize = sizeof(struct fuse_bufvec) + (sizeof(struct fuse_buf) * pcount);
    bufv = alloca(fsize);
    pages = alloca(sizeof(struct page *) * pcount);
    memset(bufv, 0, fsize);
    fs = lc_getfs(ino, false);
    inode = lc_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISREG(inode->i_mode));

    /* Reading beyond file size is not allowed */
    fsize = inode->i_size;
    if (off >= fsize) {
        lc_inodeUnlock(inode);
        fuse_reply_buf(req, NULL, 0);
        goto out;
    }
    endoffset = off + size;
    if (endoffset > fsize) {
        endoffset = fsize;
    }
    lc_readPages(req, inode, off, endoffset, pages, bufv);
    lc_inodeUnlock(inode);

out:
    lc_statsAdd(fs, LC_READ, err, &start);
    if (fs->fs_gfs->gfs_pcount > LC_PAGE_MAX) {
        lc_purgePages(fs->fs_gfs);
    }
    lc_unlock(fs);
}

/* Flush a file */
static void
lc_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct inode *inode = (struct inode *)fi->fh;

    lc_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    lc_statsAdd(inode->i_fs, LC_FLUSH, 0, NULL);
}

/* Decrement open count on an inode */
static void
lc_releaseInode(fuse_req_t req, struct fs *fs, fuse_ino_t ino,
                 struct fuse_file_info *fi, bool *inval) {
    struct inode *inode;

    assert(fi);
    inode = (struct inode *)fi->fh;

    /* Nothing to do if inode is not part of this layer */
    if (inode->i_fs != fs) {
        if (inval) {
            *inval = (inode->i_size > 0) && S_ISREG(inode->i_mode);
        }
        fuse_reply_err(req, 0);
        return;
    }
    lc_inodeLock(inode, true);
    assert(inode->i_ino == lc_getInodeHandle(ino));
    assert(inode->i_fs == fs);
    assert(inode->i_ocount > 0);
    inode->i_ocount--;

    /* Invalidate pages of shared files in kernel page cache */
    if (inval) {
        *inval = (inode->i_ocount == 0) && (inode->i_size > 0) &&
                 (!inode->i_private || fs->fs_readOnly ||
                  (fs->fs_snap != NULL));
    }
    fuse_reply_err(req, 0);

    /* Truncate a removed file on last close */
    if ((inode->i_ocount == 0) && (inode->i_flags & LC_INODE_REMOVED) &&
        S_ISREG(inode->i_mode)) {
        lc_truncate(inode, 0);
    }

    /* Flush dirty pages of a file on last close */
    if ((inode->i_ocount == 0) && (inode->i_flags & LC_INODE_EMAPDIRTY)) {
        assert(S_ISREG(inode->i_mode));
        if (fs->fs_readOnly) {

            /* Inode emap needs to be stable before an inode could be cloned */
            lc_emapFlush(fs->fs_gfs, inode->i_fs, inode);
        } else if (!(inode->i_flags & (LC_INODE_REMOVED | LC_INODE_TMP)) &&
                   inode->i_page && (inode->i_dnext == NULL) &&
                   (fs->fs_dirtyInodesLast != inode)) {
            lc_addDirtyInode(fs, inode);
        }
    }
    lc_inodeUnlock(inode);
}

/* Release open count on a file */
static void
lc_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct timeval start;
    struct fs *fs;
    bool inval;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    fs = lc_getfs(ino, false);
    lc_releaseInode(req, fs, ino, fi, &inval);
    if (inval) {
        fuse_lowlevel_notify_inval_inode(gfs->gfs_se, ino, 0, -1);
    }
    lc_statsAdd(fs, LC_RELEASE, false, &start);
    lc_unlock(fs);
}

/* Sync a file */
static void
lc_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
          struct fuse_file_info *fi) {
    struct inode *inode = (struct inode *)fi->fh;

    /* Fsync is disabled in this file system as layers are made persistent when
     * needed.
     */
    lc_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    lc_statsAdd(inode->i_fs, LC_LCYNC, 0, NULL);
}

/* Open a directory */
static void
lc_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, 0, ino, NULL);
    fs = lc_getfs(ino, false);
    err = lc_openInode(fs, ino, fi);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_open(req, fi);
    }
    lc_statsAdd(fs, LC_OPENDIR, err, &start);
    lc_unlock(fs);
}

/* Read entries from a directory */
static void
lc_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
            struct fuse_file_info *fi) {
    struct timeval start;
    struct inode *dir;
    struct stat st;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    memset(&st, 0, sizeof(struct stat));
    fs = lc_getfs(ino, false);
    dir = lc_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (dir) {
        err = lc_dirReaddir(req, fs, dir, ino, size, off, &st);
        lc_inodeUnlock(dir);
    } else {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
    }
    lc_statsAdd(fs, LC_READDIR, err, &start);
    lc_unlock(fs);
}

/* Release a directory */
static void
lc_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    fs = lc_getfs(ino, false);
    lc_releaseInode(req, fs, ino, fi, NULL);
    lc_statsAdd(fs, LC_RELEASEDIR, false, &start);
    lc_unlock(fs);
}

/* Sync a directory */
static void
lc_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
             struct fuse_file_info *fi) {
    struct inode *inode = (struct inode *)fi->fh;

    /* Fsync is disabled in this file system as layers are made persistent when
     * needed.
     */
    lc_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    lc_statsAdd(inode->i_fs, LC_LCYNCDIR, 0, NULL);
}

/* File system statfs */
static void
lc_statfs(fuse_req_t req, fuse_ino_t ino) {
    struct gfs *gfs = getfs();
    struct super *super = gfs->gfs_super;
    struct timeval start;
    struct statvfs buf;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    memset(&buf, 0, sizeof(struct statvfs));
    buf.f_bsize = LC_BLOCK_SIZE;
    buf.f_frsize = LC_BLOCK_SIZE;
    buf.f_blocks = super->sb_tblocks;
    buf.f_bfree = buf.f_blocks - super->sb_blocks;
    buf.f_bavail = buf.f_bfree;
    buf.f_files = UINT32_MAX;
    buf.f_ffree = buf.f_files - super->sb_inodes;
    buf.f_favail = buf.f_ffree;
    buf.f_namemax = LC_FILENAME_MAX;
    fuse_reply_statfs(req, &buf);
    lc_statsAdd(lc_getGlobalFs(gfs), LC_STATLC, false, &start);
}

/* Set extended attributes on a file, currently used for creating a new file
 * system
 */
static void
lc_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
             const char *value, size_t size, int flags) {
    lc_displayEntry(__func__, ino, 0, name);
    lc_xattrAdd(req, ino, name, value, size, flags);
}

/* Get extended attributes of the specified inode */
static void
lc_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
    struct gfs *gfs = getfs();

    lc_displayEntry(__func__, ino, 0, name);
    if (!gfs->gfs_xattr_enabled) {
        //lc_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        return;
    }

    /* XXX Figure out a way to avoid invoking this for system.posix_acl_access
     * and system.posix_acl_default.
     */
    lc_xattrGet(req, ino, name, size);
}

/* List extended attributes on a file */
static void
lc_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    struct gfs *gfs = getfs();

    lc_displayEntry(__func__, ino, 0, NULL);
    if (!gfs->gfs_xattr_enabled) {
        //lc_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        return;
    }
    lc_xattrList(req, ino, size);
}

/* Remove extended attributes */
static void
lc_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
    struct gfs *gfs = getfs();

    lc_displayEntry(__func__, ino, 0, name);
    if (!gfs->gfs_xattr_enabled) {
        //lc_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        return;
    }
    lc_xattrRemove(req, ino, name);
}

/* Create a file */
static void
lc_create(fuse_req_t req, fuse_ino_t parent, const char *name,
           mode_t mode, struct fuse_file_info *fi) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getfs(parent, false);
    err = create(fs, parent, name, S_IFREG | (mode & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, NULL, fi, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_create(req, &e, fi);
    }
    lc_statsAdd(fs, LC_CREATE, err, &start);
    lc_unlock(fs);
}

/* IOCTLs for certain operations.  Supported only on layer root directory */
static void
lc_ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
         struct fuse_file_info *fi, unsigned flags,
         const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    char name[in_bufsz + 1], *snap, *parent;
    struct gfs *gfs = getfs();
    int len, op;

    lc_displayEntry(__func__, ino, cmd, NULL);
    op = _IOC_NR(cmd);

    /* XXX For allowing tests to run */
    if ((op == SNAP_CREATE) && (gfs->gfs_snap_root != ino)) {
        lc_setSnapshotRoot(gfs, ino);
    }
    if (ino != gfs->gfs_snap_root) {
        //lc_reportError(__func__, __LINE__, ino, ENOSYS);
        fuse_reply_err(req, ENOSYS);
        return;
    }
    if (in_bufsz > 0) {
        memcpy(name, in_buf, in_bufsz);
    }
    name[in_bufsz] = 0;
    switch (op) {
    case SNAP_CREATE:
    case CLONE_CREATE:
        len = _IOC_TYPE(cmd);
        if (len) {
            parent = name;
            name[len] = 0;
            snap = &name[len + 1];
        } else {
            parent = "";
            len = 0;
            snap = name;
        }
        lc_newClone(req, gfs, snap, parent, len, op == CLONE_CREATE);
        return;

    case SNAP_REMOVE:
        lc_removeClone(req, gfs, name);
        return;

    case SNAP_MOUNT:
    case SNAP_STAT:
    case SNAP_UMOUNT:
    case UMOUNT_ALL:
    case CLEAR_STAT:
        lc_snapIoctl(req, gfs, name, op);
        return;

    default:
        lc_reportError(__func__, __LINE__, ino, ENOSYS);
        fuse_reply_err(req, ENOSYS);
    }
}

/* Write provided data to file at the specified offset */
static void
lc_write_buf(fuse_req_t req, fuse_ino_t ino,
              struct fuse_bufvec *bufv, off_t off, struct fuse_file_info *fi) {
    uint64_t pcount, reserved = 0;
    struct fuse_bufvec *dst;
    struct timeval start;
    struct inode *inode;
    struct dpage *dpages;
    size_t size, wsize;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    size = bufv->buf[bufv->idx].size;
    pcount = (size / LC_BLOCK_SIZE) + 2;
    wsize = sizeof(struct fuse_bufvec) + (sizeof(struct fuse_buf) * pcount);
    dst = alloca(wsize);
    memset(dst, 0, wsize);
    dpages = alloca(pcount * sizeof(struct dpage));
    fs = lc_getfs(ino, false);
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        pcount = 0;
        goto out;
    }

    /* Copy in the data before taking the lock */
    pcount = lc_copyPages(fs, off, size, dpages, bufv, dst);
    reserved = __sync_add_and_fetch(&fs->fs_pcount, pcount);
    if (!lc_hasSpace(fs->fs_gfs, reserved)) {
        lc_reportError(__func__, __LINE__, ino, ENOSPC);
        fuse_reply_err(req, ENOSPC);
        err = ENOSPC;
        goto out;
    }
    inode = lc_getInode(fs, ino, (struct inode *)fi->fh, true, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    /* Now the write cannot fail, so respond success */
    fuse_reply_write(req, size);
    assert(S_ISREG(inode->i_mode));

    /* Link the dirty pages to the inode and update times */
    pcount -= lc_addPages(inode, off, size, dpages, pcount);
    lc_updateInodeTimes(inode, true, true);
    lc_markInodeDirty(inode, true, false, true, false);
    lc_inodeUnlock(inode);

out:
    if (pcount && reserved) {
        __sync_sub_and_fetch(&fs->fs_pcount, pcount);
    }
    if (err) {
        while (pcount) {
            pcount--;
            lc_free(fs, dpages[pcount].dp_data, LC_BLOCK_SIZE,
                    LC_MEMTYPE_DATA);
        }
    }
    lc_statsAdd(fs, LC_WRITE_BUF, err, &start);
    if (!err && (fs->fs_pcount >= LC_MAX_LAYER_DIRTYPAGES)) {
        lc_flushDirtyInodeList(fs);
    }
    lc_unlock(fs);
}

/* Readdir with file attributes */
static void
lc_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
               struct fuse_file_info *fi) {
    struct timeval start;
    struct inode *dir;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    fs = lc_getfs(ino, false);
    dir = lc_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (dir) {
        err = lc_dirReaddir(req, fs, dir, ino, size, off, NULL);
        lc_inodeUnlock(dir);
    } else {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
    }
    lc_statsAdd(fs, LC_READDIRPLUS, err, &start);
    lc_unlock(fs);
}

/* Initialize a new file system */
static void
lc_init(void *userdata, struct fuse_conn_info *conn) {
    conn->want |= FUSE_CAP_SPLICE_WRITE | FUSE_CAP_SPLICE_MOVE |
                  FUSE_CAP_WRITEBACK_CACHE;
    conn->want &= ~FUSE_CAP_HANDLE_KILLPRIV;
}

/* Destroy a file system */
static void
lc_destroy(void *fsp) {
    struct gfs *gfs = (struct gfs *)fsp;

    lc_unmount(gfs);
}

/* Fuse operations registered with the fuse driver */
struct fuse_lowlevel_ops lc_ll_oper = {
    .init       = lc_init,
    .destroy    = lc_destroy,
    .lookup     = lc_lookup,
    //.forget     = lc_forget,
	.getattr	= lc_getattr,
    .setattr    = lc_setattr,
	.readlink	= lc_readlink,
	.mknod  	= lc_mknod,
	.mkdir  	= lc_mkdir,
	.unlink  	= lc_unlink,
	.rmdir		= lc_rmdir,
	.symlink	= lc_symlink,
    .rename     = lc_rename,
    .link       = lc_link,
    .open       = lc_open,
    .read       = lc_read,
    .flush      = lc_flush,
    .release    = lc_release,
    .fsync      = lc_fsync,
    .opendir    = lc_opendir,
    .readdir    = lc_readdir,
    .releasedir = lc_releasedir,
    .fsyncdir   = lc_fsyncdir,
    .statfs     = lc_statfs,
    .setxattr   = lc_setxattr,
    .getxattr   = lc_getxattr,
    .listxattr  = lc_listxattr,
    .removexattr  = lc_removexattr,
    //.access     = lc_access,
    .create     = lc_create,
#if 0
    .getlk      = lc_getlk,
    .setlk      = lc_setlk,
    .bmap       = lc_emap,
#endif
    .ioctl      = lc_ioctl,
#if 0
    .poll       = lc_poll,
#endif
    .write_buf  = lc_write_buf,
#if 0
    .retrieve_reply = lc_retrieve_reply,
    .forget_multi = lc_forget_multi,
    .flock      = lc_flock,
    .fallocate  = lc_fallocate,
#endif
    .readdirplus = lc_readdirplus,
};
