/* Capability-backed file handles for mounted filesystems. */
#include "finte/finte.h"

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            b = p + 1;
        }
    }
    return b;
}

int finte_disk_file_open(const char *path, uint32_t flags, fhandle_t *out)
{
    if (!path || path[0] != '/' || !out) {
        return FINTE_ERR_INVAL;
    }
    uint32_t access = flags & (FINTE_OPEN_READ | FINTE_OPEN_WRITE);
    if (access == 0) {
        return FINTE_ERR_INVAL;
    }
    fdfs_stat_t st;
    int rc = finte_disk_stat(path, &st);
    if (rc == FINTE_ERR_NOENT && (flags & FINTE_OPEN_CREATE)) {
        rc = finte_disk_create(path, 0);
        if (rc == 0 || rc == FINTE_ERR_EXIST) {
            rc = finte_disk_stat(path, &st);
        }
    }
    if (rc != 0) {
        return rc;
    }
    if (st.mode == FDISK_MODE_DIR) {
        return FINTE_ERR_TYPE;
    }
    if (flags & FINTE_OPEN_TRUNC) {
        if (!(flags & FINTE_OPEN_WRITE)) {
            return FINTE_ERR_PERM;
        }
        rc = finte_disk_truncate(path, 0);
        if (rc != 0) {
            return rc;
        }
        st.size = 0;
    }
    for (uint32_t i = 0; i < FINTE_MAX_FILES; i++) {
        ffile_t *f = &finte_core.files[i];
        if (f->in_use) {
            continue;
        }
        finte_memset(f, 0, sizeof(*f));
        f->in_use = 1;
        f->backend = 1;
        f->writable = (flags & FINTE_OPEN_WRITE) ? 1 : 0;
        f->rights = 0;
        if (flags & FINTE_OPEN_READ) {
            f->rights |= FINTE_R_READ;
        }
        if (flags & FINTE_OPEN_WRITE) {
            f->rights |= FINTE_R_WRITE;
        }
        f->size = (size_t)st.size;
        finte_strlcpy(f->path, path, sizeof(f->path));
        finte_strlcpy(f->name, base_name(path), sizeof(f->name));
        fhandle_t h = finte_node_alloc(FINTE_NODE_FILE, i, 0);
        if (!h) {
            f->in_use = 0;
            return FINTE_ERR_NOMEM;
        }
        finte_node_ref(h);
        *out = h;
        return 0;
    }
    return FINTE_ERR_LIMIT;
}

static ffile_t *file_get(fhandle_t h)
{
    fnode_t *n = finte_node_get(h);
    if (!n || n->type != FINTE_NODE_FILE) {
        return NULL;
    }
    return &finte_core.files[n->index];
}

int64_t finte_file_handle_read(fhandle_t h, void *buf, size_t len)
{
    ffile_t *f = file_get(h);
    if (!f || !(f->rights & FINTE_R_READ)) {
        return FINTE_ERR_PERM;
    }
    if (f->backend) {
        int64_t n = finte_disk_read(f->path, f->offset, buf, len);
        if (n > 0) {
            f->offset += (uint64_t)n;
        }
        return n;
    }
    if (f->offset >= f->size) {
        return 0;
    }
    uint64_t avail = f->size - f->offset;
    if (len > avail) {
        len = (size_t)avail;
    }
    finte_memcpy(buf, f->data + f->offset, len);
    f->offset += len;
    return (int64_t)len;
}

int64_t finte_file_handle_write(fhandle_t h, const void *buf, size_t len)
{
    ffile_t *f = file_get(h);
    if (!f || !(f->rights & FINTE_R_WRITE) || !f->backend) {
        return FINTE_ERR_PERM;
    }
    int64_t n = finte_disk_write(f->path, f->offset, buf, len);
    if (n >= 0) {
        f->offset += (uint64_t)n;
        if (f->offset > f->size) {
            f->size = (size_t)f->offset;
        }
    }
    return n;
}

int finte_file_handle_seek(fhandle_t h, uint64_t off)
{
    ffile_t *f = file_get(h);
    if (!f) {
        return FINTE_ERR_INVAL;
    }
    if (off > f->size && !(f->rights & FINTE_R_WRITE)) {
        return FINTE_ERR_INVAL;
    }
    f->offset = off;
    return 0;
}

int64_t finte_file_handle_size(fhandle_t h)
{
    ffile_t *f = file_get(h);
    if (!f) {
        return FINTE_ERR_INVAL;
    }
    if (f->backend) {
        fdfs_stat_t st;
        int rc = finte_disk_stat(f->path, &st);
        if (rc != 0) {
            return rc;
        }
        f->size = (size_t)st.size;
    }
    return (int64_t)f->size;
}

int finte_file_handle_close(fhandle_t h)
{
    if (!file_get(h)) {
        return FINTE_ERR_INVAL;
    }
    finte_node_unref(h);
    return 0;
}
