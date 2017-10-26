#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

typedef uint16_t	blkno_t;

struct filesys {
    uint16_t      s_mounted;
    uint16_t      s_isize;
    uint16_t      s_fsize;
    int16_t       s_nfree;
    blkno_t     s_free[50];
    int16_t       s_ninode;
    uint16_t      s_inode[50];
    uint8_t       s_fmod;
#define FMOD_DIRTY	1
#define FMOD_CLEAN	2
    uint8_t	s_timeh;	/* top bits of time */
    uint32_t      s_time;
    blkno_t     s_tfree;
    uint16_t      s_tinode;
    uint16_t      s_mntpt;
};

#define ROOTINODE 1
#define SMOUNTED 12742   /* Magic number to specify mounted filesystem */
#define SMOUNTED_WRONGENDIAN 50737   /* byteflipped */

struct dinode {
    uint16_t i_mode;
    uint16_t i_nlink;
    uint16_t i_uid;
    uint16_t i_gid;
    uint32_t    i_size;
    uint32_t   i_atime;
    uint32_t   i_mtime;
    uint32_t   i_ctime;
    blkno_t  i_addr[20];
};               /* Exactly 64 bytes long! */

#define F_REG   0100000
#define F_DIR   040000
#define F_PIPE  010000
#define F_BDEV  060000
#define F_CDEV  020000

#define F_MASK  0170000

struct direct {
        uint16_t   d_ino;
        char     d_name[30];
};

#define MAXDEPTH 20	/* Maximum depth of directory tree to search */

/* This checks a filesystem */

static int dev = 0;
static struct filesys superblock;
static int swizzling = 0;		/* Wrongendian ? */
static long offset;
static int dev_fd;
static int dev_offset;
static int error;
static int rootfs;
static int aflag;

static unsigned char *bitmap;
static int16_t *linkmap;
static char *daread(uint16_t blk);
static void dwrite(uint16_t blk, char *addr);
static void iread(uint16_t ino, struct dinode *buf);
static void iwrite(uint16_t ino, struct dinode *buf);
static void setblkno(struct dinode *ino, blkno_t num, blkno_t dnum);
static void ckdir(uint16_t inum, uint16_t pnum, char *name);
static void dirread(struct dinode *ino, uint16_t j, struct direct *dentry);
static void dirwrite(struct dinode *ino, uint16_t j, struct direct *dentry);
static void mkentry(uint16_t inum);
static blkno_t blk_alloc0(struct filesys *filesys);
static blkno_t getblkno(struct dinode *ino, blkno_t num);

static void pass1(void);
static void pass2(void);
static void pass3(void);
static void pass4(void);
static void pass5(void);

static int yes_noerror(void)
{
    static char buf[16];
    fflush(stdout);
    do {
        if (fgets(buf, 15, stdin) == NULL)
            exit(1);
        if (isupper(*buf))
            *buf = tolower(*buf);
    } while(*buf != 'n' && *buf != 'y');
    return  (*buf == 'y') ? 1 : 0;
}

static int yes(void) {
    int ret = yes_noerror();
    if (ret)
        error |= 1;
    else
        error |= 4;
}

static void bitset(uint16_t b)
{
    bitmap[b >> 3] |= (1 << (b & 7));
}

static void bitclear(uint16_t b)
{
    bitmap[b >> 3] &= ~(1 << (b & 7));
}

static int bittest(uint16_t b)
{
    return (bitmap[b >> 3] & (1 << (b & 7))) ? 1 : 0;
}

static void panic(char *s)
{
	fprintf(stderr, "panic: %s\n", s);
	exit(error | 8);
}

static int fd_open(char *name)
{
	char *sd;
	int bias = 0;
	struct stat rootst;
	struct stat work;

	sd = index(name, ':');
	if (sd) {
		*sd = 0;
		sd++;
		bias = atoi(sd);
	}

	printf("Opening %s (offset %d)\n", name, bias);
	dev_offset = bias;
	dev_fd = open(name, O_RDWR | O_CREAT, 0666);

	if (dev_fd < 0)
		return -1;
	/* printf("fd=%d, dev_offset = %d\n", dev_fd, dev_offset); */

	if (stat("/", &rootst) == -1)
	    panic("stat /");
        if (fstat(dev_fd, &work) == -1)
            panic("statfd");

        if (rootst.st_dev == work.st_rdev) {
            printf("Checking root file system.\n");
            rootfs = 1;
        }

	return 0;
}

static uint16_t swizzle16(uint32_t v)
{
        int top = v & 0xFFFF0000UL;
	if (top && top != 0xFFFF0000) {
		fprintf(stderr, "swizzle16 given a 32bit input\n");
		exit(error | 8);
	}
	if (swizzling)
		return (v & 0xFF) << 8 | ((v & 0xFF00) >> 8);
	else
		return v;
}

static uint32_t swizzle32(uint32_t v)
{
	if (!swizzling)
		return v;

	return (v & 0xFF) << 24 | (v & 0xFF00) << 8 | (v & 0xFF0000) >> 8 |
	    (v & 0xFF000000) >> 24;
}

int main(int argc, char **argv)
{
    char *buf;
    char *op;

    if (argc == 3 && strcmp(argv[1],"-a") == 0) {
        argc--;
        argv++;
        aflag = 1;
    }

    if(argc != 2){
        fprintf(stderr, "syntax: fsck[-a] [devfile][:offset]\n");
        return 16;
    }
    
    op = strchr(argv[1], ':');
    if (op) {
        *op++ = 0;
        offset = atol(op);
    }

    if(fd_open(argv[1])){
        printf("Cannot open file\n");
        return 16;
    }

    buf = daread(1);
    bcopy(buf, (char *) &superblock, sizeof(struct filesys));

    if (superblock.s_fmod == FMOD_DIRTY) {
        printf("Filesystem was not cleanly unmounted.\n");
        error |= 1;
    }
    else if (aflag)
        return 0;

    /* Verify the fsize and isize parameters */
    if (superblock.s_mounted == SMOUNTED_WRONGENDIAN) {
        swizzling = 1;
        printf("Checking file system with reversed byte order.\n");
    }

    if (swizzle16(superblock.s_mounted) != SMOUNTED) {
        printf("Device %d has invalid magic number %d. Fix? ", dev, superblock.s_mounted);
        if (!yes())
            exit(error|32);
        superblock.s_mounted = swizzle16(SMOUNTED);
        dwrite((blkno_t) 1, (char *) &superblock);
    }
    printf("Device %d has fsize = %d and isize = %d. Continue? ",
            dev, swizzle16(superblock.s_fsize), swizzle16(superblock.s_isize));
    if (!yes_noerror())
        exit(error | 32);

    bitmap = calloc((swizzle16(superblock.s_fsize) + 7UL) / 8, sizeof(char));
    linkmap = (int16_t *) calloc(8 * swizzle16(superblock.s_isize), sizeof(int16_t));

    printf("Memory pool %d bytes\n",
        16 * swizzle16(superblock.s_isize) +
        (swizzle16(superblock.s_fsize) + 7UL) / 8);

    if (!bitmap || !linkmap) {
        fprintf(stderr, "Not enough memory.\n");
        exit(error | 8);
    }

    printf("Pass 1: Checking inodes...\n");
    pass1();

    printf("Pass 2: Rebuilding free list...\n");
    pass2();

    printf("Pass 3: Checking block allocation...\n");
    pass3();

    printf("Pass 4: Checking directory entries...\n");
    pass4();

    printf("Pass 5: Checking link counts...\n");
    pass5();

    /* If we fixed things, and no errors were left uncorrected */
    if ((error & 5) == 1) {
        superblock.s_fmod = FMOD_CLEAN;
        dwrite((blkno_t) 1, (char *) &superblock);
        if (rootfs) {
            error |= 2;
            printf("**** Root filesystem was modified, immediate reboot required.\n");
            sleep(5);
            /* Sync it all out and reboot */
            uadmin(A_REBOOT, 0, 0);
        }
    }

    printf("Done.\n");

    exit(error);
}


/*
 *  Pass 1 checks each inode independently for validity, zaps bad block
 *  numbers in the inodes, and builds the block allocation map.
 */

static void pass1(void)
{
    uint16_t n;
    struct dinode ino;
    uint16_t mode;
    blkno_t b;
    blkno_t bno;
    uint16_t icount;
    blkno_t *buf;

    icount = 0;

    for (n = ROOTINODE; n < 8 * (swizzle16(superblock.s_isize) - 2); ++n) {
        iread(n, &ino);
        linkmap[n] = -1;
        if (ino.i_mode == 0)
            continue;

        mode = swizzle16(ino.i_mode) & F_MASK;
        /* FIXME: named pipes.. */

        /* Check mode */
        if (mode != F_REG && mode != F_DIR && mode != F_BDEV && mode != F_CDEV) {
            printf("Inode %d with mode 0%o is not of correct type. Zap? ",
                    n, swizzle16(ino.i_mode));
            if (yes()) {
                ino.i_mode = 0;
                ino.i_nlink = 0;
                iwrite(n, &ino);
                continue;
            }
        }
        linkmap[n] = 0;
        ++icount;
        /* Check size */

        if (swizzle32(ino.i_size) < 0) {
            printf("Inode %d offset is negative with value of %ld. Fix? ",
                    n, (long)swizzle32(ino.i_size));
            if (yes()) {
                ino.i_size = 0;
                iwrite(n, &ino);
            }
        }
        /* Check blocks and build free block map */
        if (mode == F_REG || mode == F_DIR) {
            /* Check singly indirect blocks */

            for (b = 18; b < 20; ++b) {
                if (ino.i_addr[b] != 0 && 
                    (swizzle16(ino.i_addr[b]) < swizzle16(superblock.s_isize) ||
                            swizzle16(ino.i_addr[b]) >= swizzle16(superblock.s_fsize))) {
                    printf("Inode %d singly ind. blk %d out of range, val = %u. Zap? ",
                            n, b, swizzle16(ino.i_addr[b]));
                    if (yes()) {
                        ino.i_addr[b] = 0;
                        iwrite(n, &ino);
                    }
                }
                if (ino.i_addr[b] != 0 && swizzle32(ino.i_size) < 18*512) {
                    printf("Inode %d singly ind. blk %d past end of file, val = %u. Zap? ",
                            n, b, swizzle16(ino.i_addr[b]));
                    if (yes()) {
                        ino.i_addr[b] = 0;
                        iwrite(n, &ino);
                    }
                }
                if (ino.i_addr[b] != 0)
                    bitset(swizzle16(ino.i_addr[b]));
            }

            /* Check the double indirect blocks */
            if (ino.i_addr[19] != 0) {
                buf = (blkno_t *) daread(swizzle16(ino.i_addr[19]));
                for (b = 0; b < 256; ++b) {
                    if (buf[b] != 0 && (swizzle16(buf[b]) < swizzle16(superblock.s_isize) ||
                                swizzle16(buf[b]) >= swizzle16(superblock.s_fsize))) {
                        printf("Inode %d doubly ind. blk %d is ", n, b);
                        printf("out of range, val = %u. Zap? ", swizzle16(buf[b]));
                        /* 1.4.98 - line split.  HFB */
                        if (yes()) {
                            buf[b] = 0;
                            dwrite(b, (char *) buf);
                        }
                    }
                    if (buf[b] != 0)
                        bitset(swizzle16(buf[b]));
                }
            }
            /* Check the rest */
            for (bno = 0; bno <= swizzle32(ino.i_size)/512; ++bno) {
                b = getblkno(&ino, bno);

                if (b != 0 && (b < swizzle16(superblock.s_isize) || b >= swizzle16(superblock.s_fsize))) {
                    printf("Inode %d block %d out of range, val = %u. Zap? ",
                            n, bno, b);
                    if (yes()) {
                        setblkno(&ino, bno, 0);
                        iwrite(n, &ino);
                    }
                }
                if (b != 0)
                    bitset(b);
            }
        }
    }
    /* Fix free inode count in superblock block */
    if (swizzle16(superblock.s_tinode) != 8 * (swizzle16(superblock.s_isize) - 2) - ROOTINODE - icount) {
        printf("Free inode count in superblock block is %u, should be %u. Fix? ",
                swizzle16(superblock.s_tinode), 8 * (swizzle16(superblock.s_isize) - 2) - ROOTINODE - icount);

        if (yes()) {
            superblock.s_tinode = swizzle16(8 * (swizzle16(superblock.s_isize) - 2) - ROOTINODE - icount);
            dwrite((blkno_t) 1, (char *) &superblock);
        }
    }
}


/* Clear inode free list, rebuild block free list using bit map. */
static void pass2(void)
{
    blkno_t j;
    blkno_t oldtfree;
    int s;

    printf("Rebuild free list? ");
    if (!yes_noerror())
        return;

    error |= 1;
    oldtfree = swizzle16(superblock.s_tfree);

    /* Initialize the superblock-block */

    superblock.s_ninode = 0;
    superblock.s_nfree = swizzle16(1);
    superblock.s_free[0] = 0;
    superblock.s_tfree = 0;

    /* Free each block, building the free list */

    for (j = swizzle16(superblock.s_fsize) - 1; j >= swizzle16(superblock.s_isize); --j) {
        if (bittest(j) == 0) {
            if (swizzle16(superblock.s_nfree) == 50) {
                dwrite(j, (char *) &superblock.s_nfree);
                superblock.s_nfree = 0;
            }
            superblock.s_tfree = swizzle16(swizzle16(superblock.s_tfree)+1);
            s = swizzle16(superblock.s_nfree);
            superblock.s_free[s++] = swizzle16(j);
            superblock.s_nfree = swizzle16(s);
        }
    }

    dwrite((blkno_t) 1, (char *) &superblock);

    if (oldtfree != swizzle16(superblock.s_tfree))
        printf("During free list regeneration s_tfree was changed to %d from %d.\n",
                swizzle16(superblock.s_tfree), oldtfree);

}

/* Pass 3 finds and fixes multiply allocated blocks. */
static void pass3(void)
{
    uint16_t n;
    struct dinode ino;
    uint16_t mode;
    blkno_t b;
    blkno_t bno;
    blkno_t newno;
    /*--- was blk_alloc ---*/

    for (b = swizzle16(superblock.s_isize); b < swizzle16(superblock.s_fsize); ++b)
        bitclear(b);

    for (n = ROOTINODE; n < 8 * (swizzle16(superblock.s_isize) - 2); ++n) {
        iread(n, &ino);

        mode = swizzle16(ino.i_mode) & F_MASK;
        if (mode != F_REG && mode != F_DIR)
            continue;

        /* Check singly indirect blocks */

        for (b = 18; b < 20; ++b) {
            if (ino.i_addr[b] != 0) {
                if (bittest(swizzle16(ino.i_addr[b])) != 0) {
                    printf("Indirect block %d in inode %u value %u multiply allocated. Fix? ",
                            b, n, swizzle16(ino.i_addr[b]));
                    if (yes()) {
                        newno = blk_alloc0(&superblock);
                        if (newno == 0) {
                            printf("Sorry... No more free blocks.\n");
                            error |= 4;
                        } else {
                            dwrite(newno, daread(swizzle16(ino.i_addr[b])));
                            ino.i_addr[b] = swizzle16(newno);
                            iwrite(n, &ino);
                        }
                    }
                } else
                    bitset(swizzle16(ino.i_addr[b]));
            }
        }

        /* Check the rest */
        for (bno = 0; bno <= swizzle32(ino.i_size)/512; ++bno) {
            b = getblkno(&ino, bno);

            if (b != 0) {
                if (bittest(b)) {
                    printf("Block %d in inode %u value %u multiply allocated. Fix? ",
                            bno, n, b);
                    if (yes()) {
                        newno = blk_alloc0(&superblock);
                        if (newno == 0) {
                            printf("Sorry... No more free blocks.\n");
                            error |= 4;
                        } else {
                            dwrite(newno, daread(b));
                            setblkno(&ino, bno, newno);
                            iwrite(n, &ino);
                        }
                    }
                } else
                    bitset(b);
            }
        }

    }

}

static int depth;

/*
 *  Pass 4 traverses the directory tree, fixing bad directory entries
 *  and finding the actual number of references to each inode.
 */

static void pass4(void)
{
    depth = 0;
    linkmap[ROOTINODE] = 1;
    ckdir(ROOTINODE, ROOTINODE, "/");
    if (depth != 0)
        panic("Inconsistent depth");
}


/* This recursively checks the directories */
static void ckdir(uint16_t inum, uint16_t pnum, char *name)
{
    struct dinode ino;
    struct direct dentry;
    uint16_t j;
    int c;
    uint8_t i;
    int nentries;
    char ename[150];

    iread(inum, &ino);
    if ((swizzle16(ino.i_mode) & F_MASK) != F_DIR)
        return;
    ++depth;

    if (swizzle32(ino.i_size) % 32 != 0) {
        printf("Directory inode %d has improper length. Fix? ", inum);
        if (yes()) {
            ino.i_size = swizzle32(swizzle32(ino.i_size) & ~0x1f);
            iwrite(inum, &ino);
        }
    }
    nentries = swizzle32(ino.i_size)/32;

    for (j = 0; j < nentries; ++j) {
        dirread(&ino, j, &dentry);

        for (i = 0; i < 30; ++i) if (dentry.d_name[i] == '\0') break;
        for (     ; i < 30; ++i) dentry.d_name[i] = '\0';
        dirwrite(&ino, j, &dentry);

        if (dentry.d_ino == 0)
            continue;

        if (swizzle16(dentry.d_ino) < ROOTINODE ||
                swizzle16(dentry.d_ino) >= 8 * swizzle16(superblock.s_isize)) {
            printf("Directory entry %s%-1.14s has out-of-range inode %u. Zap? ",
                    name, dentry.d_name, swizzle16(dentry.d_ino));
            if (yes()) {
                dentry.d_ino = 0;
                dentry.d_name[0] = '\0';
                dirwrite(&ino, j, &dentry);
                continue;
            }
        }
        if (dentry.d_ino && linkmap[swizzle16(dentry.d_ino)] == -1) {
            printf("Directory entry %s%-1.14s points to bogus inode %u. Zap? ",
                    name, dentry.d_name, swizzle16(dentry.d_ino));
            if (yes()) {
                dentry.d_ino = 0;
                dentry.d_name[0] = '\0';
                dirwrite(&ino, j, &dentry);
                continue;
            }
        }
        ++linkmap[swizzle16(dentry.d_ino)];

        for (c = 0; c < 30 && dentry.d_name[c]; ++c) {
            if (dentry.d_name[c] == '/') {
                printf("Directory entry %s%-1.30s contains slash. Fix? ",
                        name, dentry.d_name);
                if (yes()) {
                    dentry.d_name[c] = 'X';
                    dirwrite(&ino, j, &dentry);
                }
            }
        }

        if (strncmp(dentry.d_name, ".", 30) == 0 && swizzle16(dentry.d_ino) != inum) {
            printf("Dot entry %s%-1.30s points to wrong place. Fix? ",
                    name, dentry.d_name);
            if (yes()) {
                dentry.d_ino = swizzle16(inum);
                dirwrite(&ino, j, &dentry);
            }
        }
        if (strncmp(dentry.d_name, "..", 30) == 0 && swizzle16(dentry.d_ino) != pnum) {
            printf("DotDot entry %s%-1.30s points to wrong place. Fix? ",
                    name, dentry.d_name);
            if (yes()) {
                dentry.d_ino = swizzle16(pnum);
                dirwrite(&ino, j, &dentry);
            }
        }
        if (swizzle16(dentry.d_ino) != pnum &&
                swizzle16(dentry.d_ino) != inum && depth < MAXDEPTH) {
            strcpy(ename, name);
            strcat(ename, dentry.d_name);
            strcat(ename, "/");
            ckdir(swizzle16(dentry.d_ino), inum, ename);
        }
    }
    --depth;
}


/* Pass 5 compares the link counts found in pass 4 with the inodes. */
static void pass5(void)
{
    uint16_t n;
    struct dinode ino;

    for (n = ROOTINODE; n < 8 * (swizzle16(superblock.s_isize) - 2); ++n) {
        iread(n, &ino);

        if (ino.i_mode == 0) {
            if (linkmap[n] != -1)
                panic("Inconsistent linkmap");
            continue;
        }

        if (linkmap[n] == -1 && ino.i_mode != 0)
            panic("Inconsistent linkmap");

        if (linkmap[n] > 0 && swizzle16(ino.i_nlink) != linkmap[n]) {
            printf("Inode %d has link count %d should be %d. Fix? ",
                    n, swizzle16(ino.i_nlink), linkmap[n]);
            if (yes()) {
                ino.i_nlink = swizzle16(linkmap[n]);
                iwrite(n, &ino);
            }
        }

        if (linkmap[n] == 0) {
            if ((swizzle16(ino.i_mode) & F_MASK) == F_BDEV ||
                    (swizzle16(ino.i_mode) & F_MASK) == F_CDEV ||
                    (ino.i_size == 0)) {
                printf("Useless inode %d with mode 0%o has become detached. Link count is %d. Zap? ",
                        n, swizzle16(ino.i_mode), swizzle16(ino.i_nlink));
                if (yes()) {
                    ino.i_nlink = 0;
                    ino.i_mode = 0;
                    iwrite(n, &ino);
                    superblock.s_tinode =
                                swizzle16(swizzle16(superblock.s_tinode) + 1);
                    dwrite((blkno_t) 1, (char *) &superblock);
                }
            } else {
#if 0
                printf("Inode %d has become detached. Link count is %d. Fix? ",
                        n, swizzle16(ino.i_nlink));
                if (yes()) {
                    ino.i_nlink = 1;
                    iwrite(n, &ino);
                    mkentry(n);
                }
#else
                printf("Inode %d has become detached. Link count is %d. ",
                        n, swizzle16(ino.i_nlink));
                if (ino.i_nlink == 0)
                    printf("Zap? ");
                else
                    printf("Fix? ");
                if (yes()) {
                    if (ino.i_nlink == 0) {
                        ino.i_nlink = 0;
                        ino.i_mode = 0;
                        iwrite(n, &ino);
                        superblock.s_tinode =
                                swizzle16(swizzle16(superblock.s_tinode) + 1);
                        dwrite((blkno_t) 1, (char *) &superblock);
                    } else {
                        ino.i_nlink = swizzle16(1);
                        iwrite(n, &ino);
                        mkentry(n);
                    }
                }
#endif
            }
        }

    }
}


/* This makes an entry in "lost+found" for inode n */
static void mkentry(uint16_t inum)
{
    struct dinode rootino;
    struct direct dentry;
    uint16_t d;

    iread(ROOTINODE, &rootino);
    for (d = 0; d < swizzle32(rootino.i_size)/32; ++d) {
        dirread(&rootino, d, &dentry);
        if (dentry.d_ino == 0 && dentry.d_name[0] == '\0') {
            dentry.d_ino = swizzle16(inum);
            sprintf(dentry.d_name, "l+f%d", inum);
            dirwrite(&rootino, d, &dentry);
            return;
        }
    }
    printf("Sorry... No empty slots in root directory.\n");
    error |= 4;
}

/*
 *  Getblkno gets a pointer index, and a number of a block in the file.
 *  It returns the number of the block on the disk.  A value of zero
 *  means an unallocated block.
 */

static blkno_t getblkno(struct dinode *ino, blkno_t num)
{
    blkno_t indb;
    blkno_t dindb;
    blkno_t *buf;

    if (num < 18) {		/* Direct block */
        return swizzle16(ino->i_addr[num]);
    }
    if (num < 256 + 18) {	/* Single indirect */
        indb = swizzle16(ino->i_addr[18]);
        if (indb == 0)
            return (0);
        buf = (blkno_t *) daread(indb);
        return swizzle16(buf[num - 18]);
    }
    /* Double indirect */
    indb = swizzle16(ino->i_addr[19]);
    if (indb == 0)
        return (0);

    buf = (blkno_t *) daread(indb);

    dindb = swizzle16(buf[(num - (18 + 256)) >> 8]);
    buf = (blkno_t *) daread(dindb);

    return swizzle16(buf[(num - (18 + 256)) & 0x00ff]);
}


/*
 *  Setblkno sets the given block number of the given file to the given
 *  disk block number, possibly creating or modifiying the indirect blocks.
 *  A return of zero means there were no blocks available to create an 
 *  indirect block. This should never happen in fsck.
 */
static void setblkno(struct dinode *ino, blkno_t num, blkno_t dnum)
{
    blkno_t indb;
    blkno_t dindb;
    blkno_t *buf;
    /*--    char *zerobuf();--*/


    if (num < 18) {			/* Direct block */
        ino->i_addr[num] = swizzle16(dnum);
    } else if (num < 256 + 18) {	/* Single indirect */
        indb = swizzle16(ino->i_addr[18]);
        if (indb == 0)
            panic("Missing indirect block");

        buf = (blkno_t *) daread(indb);
        buf[num - 18] = swizzle16(dnum);
        dwrite(indb, (char *) buf);
    } else {				/* Double indirect */
        indb = swizzle16(ino->i_addr[19]);
        if (indb == 0)
            panic("Missing indirect block");

        buf = (blkno_t *) daread(indb);
        dindb = swizzle16(buf[(num - (18 + 256)) >> 8]);
        if (dindb == 0)
            panic("Missing indirect block");

        buf = (blkno_t *) daread(dindb);
        buf[(num - (18 + 256)) & 0x00ff] = swizzle16(num);
        dwrite(indb, (char *) buf);
    }
}


/*
 *  blk_alloc0 allocates an unused block.
 *  A returned block number of zero means no more blocks.
 */

/*--- was blk_alloc ---*/
static blkno_t blk_alloc0(struct filesys *filesys)
{
    blkno_t newno;
    blkno_t *buf;
    int16_t j;

    filesys->s_nfree = swizzle16(swizzle16(filesys->s_nfree) - 1);
    newno = swizzle16(filesys->s_free[--filesys->s_nfree]);
    if (!newno) {
        filesys->s_nfree = swizzle16(swizzle16(filesys->s_nfree) + 1);
        return (0);
    }

    /* See if we must refill the s_free array */

    if (!filesys->s_nfree) {
        buf = (blkno_t *) daread(newno);
        filesys->s_nfree = buf[0];
        for (j = 0; j < 50; j++) {
            filesys->s_free[j] = buf[j + 1];
        }
    }

    filesys->s_tfree = swizzle16(swizzle16(filesys->s_tfree) - 1);

    if (newno < swizzle16(filesys->s_isize) || newno >= swizzle16(filesys->s_fsize)) {
        printf("Free list is corrupt.  Did you rebuild it?\n");
        return (0);
    }
    dwrite((blkno_t) 1, (char *) filesys);
    return (newno);
}

static char *daread(uint16_t blk)
{
    static char da_buf[512];
    if (lseek(dev_fd, offset + blk * 512L, 0) == -1) {
        perror("lseek");
        exit(1);
    }
    if(read(dev_fd, da_buf, 512) != 512) {
        perror("read");
        exit(1);
    }
    return da_buf;
}

static void dwrite(uint16_t blk, char *addr)
{
    if (lseek(dev_fd, offset + blk * 512L, 0) == -1) {
        perror("lseek");
        exit(1);
    }
    if(write(dev_fd, addr, 512) != 512) {
        perror("write");
        exit(1);
    }
}

static void iread(uint16_t ino, struct dinode *buf)
{
    struct dinode *addr;

    addr = (struct dinode *) daread((ino >> 3) + 2);
    bcopy((char *) &addr[ino & 7], (char *) buf, sizeof(struct dinode));
}

static void iwrite(uint16_t ino, struct dinode *buf)
{
    struct dinode *addr;

    addr = (struct dinode *) daread((ino >> 3) + 2);
    bcopy((char *) buf, (char *) &addr[ino & 7], sizeof(struct dinode));
    dwrite((ino >> 3) + 2, (char *) addr);
}

static void dirread(struct dinode *ino, uint16_t j, struct direct *dentry)
{
    blkno_t blkno;
    char *buf;

    blkno = getblkno(ino, (blkno_t) j / 16);
    if (blkno == 0)
        panic("Missing block in directory");
    buf = daread(blkno);
    bcopy(buf + 32 * (j % 16), (char *) dentry, 32);
}

static void dirwrite(struct dinode *ino, uint16_t j, struct direct *dentry)
{
    blkno_t blkno;
    char *buf;

    blkno = getblkno(ino, (blkno_t) j / 16);
    if (blkno == 0)
        panic("Missing block in directory");
    buf = daread(blkno);
    bcopy((char *) dentry, buf + 32 * (j % 16), 32);
    dwrite(blkno, buf);
}

