#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#define BLOCK_SIZE 4096U

// Fixed layout (matches mkfs/validator)
#define SUPERBLOCK_BLK     0U
#define JOURNAL_START_BLK  1U
#define JOURNAL_BLOCKS     16U
#define INODE_BITMAP_BLK   (JOURNAL_START_BLK + JOURNAL_BLOCKS)  // 17
#define DATA_BITMAP_BLK    (INODE_BITMAP_BLK + 1U)               // 18
#define INODE_TABLE_BLK    (DATA_BITMAP_BLK + 1U)                // 19 (2 blocks: 19,20)
#define INODE_TABLE_BLOCKS 2U
#define DATA_START_BLK     (INODE_TABLE_BLK + INODE_TABLE_BLOCKS) // 21
#define DATA_BLOCKS        64U

#define INODE_SIZE 128U
#define INODES_PER_BLOCK (BLOCK_SIZE / INODE_SIZE)
#define INODE_COUNT (INODE_TABLE_BLOCKS * INODES_PER_BLOCK)

#define DIRECT_POINTERS 8U

// Journal format (internal to our tool; validator doesn't check journal contents)
#define JOURNAL_MAGIC 0xdeadbeefU
#define JOURNAL_BYTES (JOURNAL_BLOCKS * BLOCK_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t nbytes; // bytes used inside the whole journal region (contiguous)
} journal_header_t;

typedef struct {
    uint32_t type;
    uint32_t size;   // total size of this record including this header
} rec_header_t;

#define REC_DATA   1U
#define REC_COMMIT 2U

// On-disk structures (must match mkfs.c / validator.c)
struct inode {
    uint16_t type;   // 0 free, 1 file, 2 dir
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t  _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[28];
};

_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");

#define DATA_REC_SIZE   (sizeof(rec_header_t) + sizeof(uint32_t) + BLOCK_SIZE)
#define COMMIT_REC_SIZE (sizeof(rec_header_t))

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void read_block(int fd, uint32_t block_no, void *buf) {
    off_t off = (off_t)block_no * BLOCK_SIZE;
    if (pread(fd, buf, BLOCK_SIZE, off) != (ssize_t)BLOCK_SIZE) die("pread");
}

static void write_block(int fd, uint32_t block_no, const void *buf) {
    off_t off = (off_t)block_no * BLOCK_SIZE;
    if (pwrite(fd, buf, BLOCK_SIZE, off) != (ssize_t)BLOCK_SIZE) die("pwrite");
}

static int bitmap_test(const uint8_t *bm, uint32_t idx) {
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

static void bitmap_set(uint8_t *bm, uint32_t idx) {
    bm[idx / 8] |= (uint8_t)(1U << (idx % 8));
}

static void load_journal(int fd, unsigned char *jbuf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
        read_block(fd, JOURNAL_START_BLK + i, jbuf + i * BLOCK_SIZE);
    }
}

static void flush_journal(int fd, const unsigned char *jbuf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
        write_block(fd, JOURNAL_START_BLK + i, jbuf + i * BLOCK_SIZE);
    }
}

static void journal_init_if_needed(unsigned char *jbuf) {
    journal_header_t *jh = (journal_header_t *)jbuf;
    if (jh->magic != JOURNAL_MAGIC || jh->nbytes < sizeof(journal_header_t) || jh->nbytes > JOURNAL_BYTES) {
        memset(jbuf, 0, JOURNAL_BYTES);
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes = (uint32_t)sizeof(journal_header_t);
    }
}

static void journal_append_data(unsigned char *jbuf, uint32_t *p_off, uint32_t block_no, const void *block_img) {
    uint32_t off = *p_off;
    rec_header_t rh = { .type = REC_DATA, .size = (uint32_t)DATA_REC_SIZE };

    memcpy(jbuf + off, &rh, sizeof(rh));
    off += (uint32_t)sizeof(rh);

    memcpy(jbuf + off, &block_no, sizeof(block_no));
    off += (uint32_t)sizeof(block_no);

    memcpy(jbuf + off, block_img, BLOCK_SIZE);
    off += BLOCK_SIZE;

    *p_off = off;
}

static void journal_append_commit(unsigned char *jbuf, uint32_t *p_off) {
    uint32_t off = *p_off;
    rec_header_t rh = { .type = REC_COMMIT, .size = (uint32_t)COMMIT_REC_SIZE };
    memcpy(jbuf + off, &rh, sizeof(rh));
    off += (uint32_t)sizeof(rh);
    *p_off = off;
}

/* -------------------- install -------------------- */
static void cmd_install(int fd) {
    unsigned char *jbuf = (unsigned char *)malloc(JOURNAL_BYTES);
    if (!jbuf) die("malloc journal");

    load_journal(fd, jbuf);
    journal_init_if_needed(jbuf);

    journal_header_t *jh = (journal_header_t *)jbuf;

    uint32_t start = (uint32_t)sizeof(journal_header_t);
    uint32_t end   = jh->nbytes;
    if (end > JOURNAL_BYTES) end = JOURNAL_BYTES;

    typedef struct {
        uint32_t block_no;
        unsigned char *block_img; // points inside jbuf
    } pending_t;

    pending_t pending[128];
    int pending_cnt = 0;

    uint32_t off = start;
    int applied = 0;

    while (off + sizeof(rec_header_t) <= end) {
        rec_header_t *rh = (rec_header_t *)(jbuf + off);

        if (rh->size < sizeof(rec_header_t)) break;
        if (off + rh->size > end) break;

        if (rh->type == REC_DATA) {
            if (rh->size != DATA_REC_SIZE) break;

            uint32_t *bno_ptr = (uint32_t *)(jbuf + off + sizeof(rec_header_t));
            unsigned char *blk_img = (unsigned char *)(jbuf + off + sizeof(rec_header_t) + sizeof(uint32_t));

            if (pending_cnt >= 128) break;
            pending[pending_cnt].block_no = *bno_ptr;
            pending[pending_cnt].block_img = blk_img;
            pending_cnt++;

            off += rh->size;

        } else if (rh->type == REC_COMMIT) {
            if (rh->size != COMMIT_REC_SIZE) break;

            // Apply committed txn
            for (int i = 0; i < pending_cnt; i++) {
                write_block(fd, pending[i].block_no, pending[i].block_img);
            }
            applied++;
            pending_cnt = 0;

            off += rh->size;

        } else {
            break; // unknown record type
        }
    }

    // Clear journal after install
    memset(jbuf, 0, JOURNAL_BYTES);
    jh = (journal_header_t *)jbuf;
    jh->magic = JOURNAL_MAGIC;
    jh->nbytes = (uint32_t)sizeof(journal_header_t);
    flush_journal(fd, jbuf);

    free(jbuf);
    printf("install: applied %d committed transaction(s), cleared journal\n", applied);
}

/* -------------------- create -------------------- */
static void cmd_create(int fd, const char *name) {
    // Basic filename rules: must fit in dirent.name (28 incl null)
    if (!name || name[0] == '\0') {
        fprintf(stderr, "create: empty name not allowed\n");
        exit(1);
    }
    if (strlen(name) >= 28) {
        fprintf(stderr, "create: name too long (max 27 chars)\n");
        exit(1);
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        fprintf(stderr, "create: invalid name\n");
        exit(1);
    }

    // Read inode bitmap
    uint8_t inode_bm[BLOCK_SIZE];
    read_block(fd, INODE_BITMAP_BLK, inode_bm);

    // Find a free inode (skip 0, root)
    int new_ino = -1;
    for (uint32_t i = 1; i < INODE_COUNT; i++) {
        if (!bitmap_test(inode_bm, i)) { new_ino = (int)i; break; }
    }
    if (new_ino < 0) {
        fprintf(stderr, "create: no free inode available\n");
        exit(1);
    }

    // Read inode table blocks
    uint8_t itbl0[BLOCK_SIZE], itbl1[BLOCK_SIZE];
    read_block(fd, INODE_TABLE_BLK + 0, itbl0);
    read_block(fd, INODE_TABLE_BLK + 1, itbl1);

    struct inode *inodes0 = (struct inode *)itbl0;
    struct inode *inodes1 = (struct inode *)itbl1;

    // Root inode is inode 0
    struct inode root = inodes0[0];

    if (root.type != 2) {
        fprintf(stderr, "create: root inode is not a directory\n");
        exit(1);
    }
    if (root.direct[0] == 0) {
        fprintf(stderr, "create: root directory has no data block\n");
        exit(1);
    }

    uint32_t root_dir_blk = root.direct[0];

    // Read root directory block
    uint8_t dirblk[BLOCK_SIZE];
    read_block(fd, root_dir_blk, dirblk);
    struct dirent *des = (struct dirent *)dirblk;

    // Check name not already present within current size
    uint32_t used_entries = root.size / sizeof(struct dirent);
    for (uint32_t i = 0; i < used_entries; i++) {
        if (des[i].inode != 0 && strncmp(des[i].name, name, sizeof(des[i].name)) == 0) {
            fprintf(stderr, "create: file already exists\n");
            exit(1);
        }
    }

    // Append new entry at the end of directory "used region"
    if (root.size + sizeof(struct dirent) > BLOCK_SIZE) {
        fprintf(stderr, "create: root directory is full (needs new data block; not implemented)\n");
        exit(1);
    }

    uint32_t new_entry_idx = used_entries;
    memset(&des[new_entry_idx], 0, sizeof(struct dirent));
    des[new_entry_idx].inode = (uint32_t)new_ino;
    strncpy(des[new_entry_idx].name, name, sizeof(des[new_entry_idx].name) - 1);
    des[new_entry_idx].name[sizeof(des[new_entry_idx].name) - 1] = '\0';

    // Update root inode size + mtime
    time_t now = time(NULL);
    root.size += (uint32_t)sizeof(struct dirent);
    root.mtime = (uint32_t)now;

    // Build the new inode
    struct inode newinode;
    memset(&newinode, 0, sizeof(newinode));
    newinode.type  = 1; // regular file
    newinode.links = 1; // referenced once from root directory
    newinode.size  = 0; // empty file, no data blocks
    newinode.ctime = (uint32_t)now;
    newinode.mtime = (uint32_t)now;

    // Put updated root inode back into inode table block 0
    inodes0[0] = root;

    // Put new inode into correct inode table block
    if ((uint32_t)new_ino < INODES_PER_BLOCK) {
        inodes0[new_ino] = newinode;
    } else {
        uint32_t idx = (uint32_t)new_ino - INODES_PER_BLOCK;
        inodes1[idx] = newinode;
    }

    // Update inode bitmap
    bitmap_set(inode_bm, (uint32_t)new_ino);

    // ---------------- journal append (inode bitmap + inode table block(s) + root dir block) ----------------
    unsigned char *jbuf = (unsigned char *)malloc(JOURNAL_BYTES);
    if (!jbuf) die("malloc journal");
    load_journal(fd, jbuf);
    journal_init_if_needed(jbuf);

    journal_header_t *jh = (journal_header_t *)jbuf;
    uint32_t off = jh->nbytes;

    // We will write these blocks:
    //  - inode bitmap block
    //  - inode table block 0 (always, because root inode changed; and maybe new inode too)
    //  - inode table block 1 (only if new inode is in block1)
    //  - root directory data block
    uint32_t needed = 0;
    needed += DATA_REC_SIZE; // inode bitmap
    needed += DATA_REC_SIZE; // inode table block 0
    if ((uint32_t)new_ino >= INODES_PER_BLOCK) needed += DATA_REC_SIZE; // inode table block 1
    needed += DATA_REC_SIZE; // root dir block
    needed += COMMIT_REC_SIZE;

    if (off + needed > JOURNAL_BYTES) {
        free(jbuf);
        fprintf(stderr, "create: journal is full; run ./journal install first\n");
        exit(1);
    }

    journal_append_data(jbuf, &off, INODE_BITMAP_BLK, inode_bm);
    journal_append_data(jbuf, &off, INODE_TABLE_BLK + 0, itbl0);
    if ((uint32_t)new_ino >= INODES_PER_BLOCK) {
        journal_append_data(jbuf, &off, INODE_TABLE_BLK + 1, itbl1);
    }
    journal_append_data(jbuf, &off, root_dir_blk, dirblk);
    journal_append_commit(jbuf, &off);

    jh->nbytes = off;
    flush_journal(fd, jbuf);
    free(jbuf);

    printf("create: logged creation of '%s' as inode %d (journaled, not installed yet)\n", name, new_ino);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage:\n  %s create <name>\n  %s install\n", argv[0], argv[0]);
        return 1;
    }

    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) die("open vsfs.img");

    if (strcmp(argv[1], "create") == 0) {
        if (argc != 3) {
            fprintf(stderr, "create requires a filename\n");
            return 1;
        }
        cmd_create(fd, argv[2]);
    } else if (strcmp(argv[1], "install") == 0) {
        cmd_install(fd);
    } else {
        fprintf(stderr, "unknown command '%s'\n", argv[1]);
        return 1;
    }

    close(fd);
    return 0;
}
