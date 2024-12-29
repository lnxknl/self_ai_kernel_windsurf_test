#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Typedefs to match original code */
typedef unsigned char uch;
typedef unsigned short ush;
typedef unsigned long ulg;

/* Debug macros */
#define DEBG(x)
#define DEBG1(x)

/* Memory management */
#define INIT

/* Window size must be at least 32K for deflated file */
#define WSIZE 0x8000

/* Constants for Huffman tree building */
#define BMAX 16         /* maximum bit length of any code */
#define N_MAX 288       /* maximum number of codes in any set */
#define lbits 9          /* bits in base literal/length lookup table */
#define dbits 6          /* bits in base distance lookup table */

/* Global variables */
static unsigned wp;            /* Current position in slide */
static uch slide[WSIZE];      /* Sliding window buffer */
static unsigned insize;        /* Valid bytes in inbuf */
static unsigned inptr;         /* Current position in input buffer */
static uch *inbuf;            /* Input buffer */
static unsigned long bb;       /* Bit buffer */
static unsigned bk;           /* Bits in bit buffer */
static unsigned hufts;        /* Track memory usage */

/* Bit input macros */
#define NEXTBYTE()  (inptr < insize ? inbuf[inptr++] : -1)
#define NEEDBITS(n) {while(k<(n)){int c=NEXTBYTE();if(c==-1)return 1;b|=((ulg)c)<<k;k+=8;}}
#define DUMPBITS(n) {b>>=(n);k-=(n);}

/* Huffman code lookup table entry */
struct huft {
    uch e;                    /* Number of extra bits or operation */
    uch b;                    /* Number of bits in this code or subcode */
    union {
        ush n;                /* Literal, length base, or distance base */
        struct huft *t;       /* Pointer to next level of table */
    } v;
};

/* Output window functions */
static void flush_window(void)
{
    if (wp > 0) {
        if (fwrite(slide, 1, wp, stdout) != wp) {
            fprintf(stderr, "Error writing output\n");
            exit(1);
        }
        wp = 0;
    }
}

/* Tables for deflate from PKZIP's appnote.txt */
static const unsigned border[] = {    /* Order of the bit length code lengths */
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static const ush cplens[] = {         /* Copy lengths for literal codes 257..285 */
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};

static const ush cplext[] = {         /* Extra bits for literal codes 257..285 */
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 99, 99}; /* 99==invalid */

static const ush cpdist[] = {         /* Copy offsets for distance codes 0..29 */
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};

static const ush cpdext[] = {         /* Extra bits for distance codes */
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
    12, 12, 13, 13};

static const unsigned mask_bits[] = {
    0x0000,
    0x0001, 0x0003, 0x0007, 0x000f, 0x001f, 0x003f, 0x007f, 0x00ff,
    0x01ff, 0x03ff, 0x07ff, 0x0fff, 0x1fff, 0x3fff, 0x7fff, 0xffff
};

/* Function prototypes */
static int huft_build(unsigned *b, unsigned n, unsigned s, const ush *d,
                     const ush *e, struct huft **t, int *m);
static int huft_free(struct huft *t);
static int inflate_codes(struct huft *tl, struct huft *td, int bl, int bd);
static int inflate_stored(void);
static int inflate_fixed(void);
static int inflate_dynamic(void);
static int inflate_block(int *e);
static int inflate(void);

/* Output buffer and functions */
#define OUTBUFSIZE 32768
static unsigned char outbuf[OUTBUFSIZE];
static unsigned outcnt = 0;

static void flush_output(void)
{
    if (outcnt) {
        fwrite(outbuf, 1, outcnt, stdout);
        outcnt = 0;
    }
}

static void write_output(int c)
{
    outbuf[outcnt++] = c;
    if (outcnt == OUTBUFSIZE)
        flush_output();
}

/* Build Huffman tables */
static int huft_build(unsigned *b, unsigned n, unsigned s, const ush *d,
                     const ush *e, struct huft **t, int *m)
{
    unsigned a;                   /* counter for codes of length k */
    unsigned f;                   /* i repeats in table every f entries */
    int g;                        /* maximum code length */
    int h;                        /* table level */
    register unsigned i;          /* counter, current code */
    register unsigned j;          /* counter */
    register int k;               /* number of bits in current code */
    int l;                        /* bits per table (returned in m) */
    register unsigned *p;         /* pointer into c[], b[], or v[] */
    register struct huft *q;      /* points to current table */
    struct huft r;                /* table entry for structure assignment */
    register int w;               /* bits before this table == (l * h) */
    unsigned *xp;                 /* pointer into x */
    int y;                        /* number of dummy codes added */
    unsigned z;                   /* number of entries in current table */

    /* Allocate work area */
    unsigned c[BMAX+1];          /* bit length count table */
    struct huft *u[BMAX];        /* table stack */
    unsigned v[N_MAX];           /* values in order of bit length */
    unsigned x[BMAX+1];          /* bit offsets, then code stack */

    /* Generate counts for each bit length */
    memset(c, 0, sizeof(c));
    p = b;  
    i = n;
    do {
        c[*p]++;                    /* assume all entries <= BMAX */
        p++;                      
    } while (--i);
    
    if (c[0] == n) {               /* null input--all zero length codes */
        *t = NULL;
        *m = 0;
        return 2;
    }

    /* Find minimum and maximum length, bound *m by those */
    l = *m;
    for (j = 1; j <= BMAX; j++)
        if (c[j])
            break;
    k = j;                        /* minimum code length */
    if ((unsigned)l < j)
        l = j;
    for (i = BMAX; i; i--)
        if (c[i])
            break;
    g = i;                        /* maximum code length */
    if ((unsigned)l > i)
        l = i;
    *m = l;

    /* Adjust last length count to fill out codes, if needed */
    for (y = 1 << j; j < i; j++, y <<= 1)
        if ((y -= c[j]) < 0)
            return 2;                 /* bad input: more codes than bits */
    if ((y -= c[i]) < 0)
        return 2;
    c[i] += y;

    /* Generate starting offsets into the value table for each length */
    x[1] = j = 0;
    p = c + 1;  
    xp = x + 2;
    while (--i)                   /* note that i == g from above */
        *xp++ = (j += *p++);

    /* Make a table of values in order of bit lengths */
    p = b;  
    i = 0;
    do {
        if ((j = *p++) != 0)
            v[x[j]++] = i;
    } while (++i < n);

    /* Generate the Huffman codes and for each, make the table entries */
    x[0] = i = 0;                 /* first Huffman code is zero */
    p = v;                        /* grab values in bit order */
    h = -1;                       /* no tables yet--level -1 */
    w = -l;                       /* bits decoded == (l * h) */
    u[0] = NULL;                  /* just to keep compilers happy */
    q = NULL;                     /* ditto */
    z = 0;                        /* ditto */

    /* go through the bit lengths (k already is bits in shortest code) */
    for (; k <= g; k++) {
        a = c[k];
        while (a--) {
            /* here i is the Huffman code of length k bits for value *p */
            /* make tables up to required level */
            while (k > w + l) {
                h++;
                w += l;                 /* previous table always l bits */

                /* compute minimum size table less than or equal to l bits */
                z = (z = g - w) > (unsigned)l ? l : z;  /* upper limit */
                if ((f = 1 << (j = k - w)) > a + 1) {   /* try a k-w bit table */
                    f -= a + 1;           /* deduct codes from patterns left */
                    xp = c + k;
                    if (j < z)
                        while (++j < z) { /* try smaller tables up to z bits */
                            if ((f <<= 1) <= *++xp)
                                break;    /* enough codes to use up j bits */
                            f -= *xp;     /* else deduct codes from patterns */
                        }
                }
                z = 1 << j;             /* table entries for j-bit table */

                /* allocate and link in new table */
                q = (struct huft *)malloc((z + 1) * sizeof(struct huft));
                if (!q)
                    return 3;            /* not enough memory */

                hufts += z + 1;         /* track memory usage */
                *t = q + 1;             /* link to list for huft_free() */
                *(t = &(q->v.t)) = NULL;
                u[h] = ++q;             /* table starts after link */

                /* connect to last table, if there is one */
                if (h) {
                    x[h] = i;           /* save pattern for backing up */
                    r.b = (uch)l;       /* bits to dump before this table */
                    r.e = (uch)(16 + j);/* bits in this table */
                    r.v.t = q;          /* pointer to this table */
                    j = i >> (w - l);   /* (get around Turbo C bug) */
                    u[h-1][j] = r;      /* connect to last table */
                }
            }

            /* set up table entry in r */
            r.b = (uch)(k - w);
            if (p >= v + n)
                r.e = 99;               /* out of values--invalid code */
            else if (*p < s) {
                r.e = (uch)(*p < 256 ? 16 : 15);  /* 256 is end-of-block code */
                r.v.n = (ush)(*p);                /* simple code is just the value */
                p++;                              /* one compiler does not like *p++ */
            } else {
                r.e = (uch)e[*p - s];   /* non-simple--look up in lists */
                r.v.n = d[*p++ - s];
            }

            /* fill code-like entries with r */
            f = 1 << (k - w);
            for (j = i >> w; j < z; j += f)
                q[j] = r;

            /* backwards increment the k-bit code i */
            for (j = 1 << (k - 1); i & j; j >>= 1)
                i ^= j;
            i ^= j;

            /* backup over finished tables */
            while ((i & ((1 << w) - 1)) != x[h]) {
                h--;                    /* don't need to update q */
                w -= l;
            }
        }
    }

    return 0;
}

/* Free the malloc'ed tables built by huft_build() */
static int huft_free(struct huft *t)
{
    struct huft *p, *q;

    /* Go through linked list, freeing from the allocated (t[-1]) address */
    p = t;
    while (p != NULL) {
        q = (--p)->v.t;
        free(p);
        p = q;
    }
    return 0;
}

/* Inflate codes */
static int inflate_codes(struct huft *tl, struct huft *td, int bl, int bd)
{
    register unsigned e;  /* table entry flag/number of extra bits */
    unsigned n, d;        /* length and index for copy */
    unsigned w;          /* current window position */
    struct huft *t;      /* pointer to table entry */
    unsigned ml, md;     /* masks for bl and bd bits */
    register ulg b;      /* bit buffer */
    register unsigned k;  /* number of bits in bit buffer */

    /* make local copies of globals */
    b = bb;                       /* initialize bit buffer */
    k = bk;
    w = wp;                       /* initialize window position */

    /* inflate the coded data */
    ml = mask_bits[bl];           /* precompute masks for speed */
    md = mask_bits[bd];
    while (1) {                   /* do until end of block */
        NEEDBITS((unsigned)bl)
        t = tl + ((unsigned)b & ml);
        e = t->e;
        if (e > 16)
            do {
                if (e == 99)
                    return 1;
                DUMPBITS(t->b)
                e -= 16;
                NEEDBITS(e)
                t = t->v.t + ((unsigned)b & mask_bits[e]);
                e = t->e;
            } while (e > 16);
        DUMPBITS(t->b)
        if (e == 16) {              /* then it's a literal */
            slide[w++] = (uch)t->v.n;
            if (w == WSIZE) {
                flush_window();
                w = 0;
            }
        }
        else {                      /* it's an EOB or a length */
            if (e == 15)           /* end of block */
                break;

            /* get length of block to copy */
            NEEDBITS(e)
            n = t->v.n + ((unsigned)b & mask_bits[e]);
            DUMPBITS(e)

            /* decode distance of block to copy */
            NEEDBITS((unsigned)bd)
            t = td + ((unsigned)b & md);
            e = t->e;
            if (e > 16)
                do {
                    if (e == 99)
                        return 1;
                    DUMPBITS(t->b)
                    e -= 16;
                    NEEDBITS(e)
                    t = t->v.t + ((unsigned)b & mask_bits[e]);
                    e = t->e;
                } while (e > 16);
            DUMPBITS(t->b)
            NEEDBITS(e)
            d = w - t->v.n - ((unsigned)b & mask_bits[e]);
            DUMPBITS(e)

            /* do the copy */
            do {
                n -= (e = (e = WSIZE - ((d &= WSIZE-1) > w ? d : w)) > n ? n : e);
                if (w - d >= e) {    /* (this test assumes unsigned comparison) */
                    memcpy(slide + w, slide + d, e);
                    w += e;
                    d += e;
                }
                else {                  /* do it slow to avoid memcpy() overlap */
                    do {
                        slide[w++] = slide[d++];
                    } while (--e);
                }
                if (w == WSIZE) {
                    flush_window();
                    w = 0;
                }
            } while (n);
        }
    }

    /* restore the globals from the locals */
    wp = w;                       /* restore global window pointer */
    bb = b;                       /* restore global bit buffer */
    bk = k;

    return 0;
}

/* Copy stored block */
static int inflate_stored(void)
{
    unsigned n;    /* number of bytes in block */
    unsigned w;    /* current window position */
    register ulg b;/* bit buffer */
    register unsigned k;  /* number of bits in bit buffer */

    /* make local copies of globals */
    b = bb;                       /* initialize bit buffer */
    k = bk;
    w = wp;                       /* initialize window position */

    /* go to byte boundary */
    n = k & 7;
    DUMPBITS(n);

    /* get the length and its complement */
    NEEDBITS(16)
    n = ((unsigned)b & 0xffff);
    DUMPBITS(16)
    NEEDBITS(16)
    if (n != (unsigned)((~b) & 0xffff))
        return 1;                   /* error in compressed data */
    DUMPBITS(16)

    /* read and output the compressed data */
    while (n--) {
        NEEDBITS(8)
        slide[w++] = (uch)b;
        if (w == WSIZE) {
            flush_window();
            w = 0;
        }
        DUMPBITS(8)
    }

    /* restore the globals from the locals */
    wp = w;                       /* restore global window pointer */
    bb = b;                       /* restore global bit buffer */
    bk = k;
    return 0;
}

/* Decompress fixed Huffman codes */
static int inflate_fixed(void)
{
    int i;                /* temporary variable */
    struct huft *tl;     /* literal/length code table */
    struct huft *td;     /* distance code table */
    int bl;             /* lookup bits for tl */
    int bd;             /* lookup bits for td */
    unsigned l[288];     /* length list for huft_build */

    /* set up literal table */
    for (i = 0; i < 144; i++)
        l[i] = 8;
    for (; i < 256; i++)
        l[i] = 9;
    for (; i < 280; i++)
        l[i] = 7;
    for (; i < 288; i++)    /* make a complete, but wrong code set */
        l[i] = 8;
    bl = 7;
    if ((i = huft_build(l, 288, 257, cplens, cplext, &tl, &bl)) != 0)
        return i;

    /* set up distance table */
    for (i = 0; i < 30; i++)      /* make an incomplete code set */
        l[i] = 5;
    bd = 5;
    if ((i = huft_build(l, 30, 0, cpdist, cpdext, &td, &bd)) > 1) {
        huft_free(tl);
        return i;
    }

    /* decompress until an end-of-block code */
    i = inflate_codes(tl, td, bl, bd);
    huft_free(tl);
    huft_free(td);
    return i;
}

/* Decompress dynamic Huffman codes */
static int inflate_dynamic(void)
{
    int i;                /* temporary variables */
    unsigned j;
    unsigned l;           /* last length */
    unsigned m;           /* mask for bit lengths table */
    unsigned n;           /* number of lengths to get */
    struct huft *tl;     /* literal/length code table */
    struct huft *td;     /* distance code table */
    int bl;             /* lookup bits for tl */
    int bd;             /* lookup bits for td */
    unsigned nb;         /* number of bit length codes */
    unsigned nl;         /* number of literal/length codes */
    unsigned nd;         /* number of distance codes */
    unsigned ll[286+30]; /* literal/length and distance code lengths */
    register ulg b;      /* bit buffer */
    register unsigned k;  /* number of bits in bit buffer */

    /* make local bit buffer */
    b = bb;
    k = bk;

    /* read in table lengths */
    NEEDBITS(5)
    nl = 257 + ((unsigned)b & 0x1f);      /* number of literal/length codes */
    DUMPBITS(5)
    NEEDBITS(5)
    nd = 1 + ((unsigned)b & 0x1f);        /* number of distance codes */
    DUMPBITS(5)
    NEEDBITS(4)
    nb = 4 + ((unsigned)b & 0xf);         /* number of bit length codes */
    DUMPBITS(4)
    if (nl > 286 || nd > 30)
        return 1;                   /* bad lengths */

    /* read in bit-length-code lengths */
    for (j = 0; j < nb; j++) {
        NEEDBITS(3)
        ll[border[j]] = (unsigned)b & 7;
        DUMPBITS(3)
    }
    for (; j < 19; j++)
        ll[border[j]] = 0;

    /* build decoding table for trees--single level, 7 bit lookup */
    bl = 7;
    if ((i = huft_build(ll, 19, 19, NULL, NULL, &tl, &bl)) != 0) {
        if (i == 1)
            huft_free(tl);
        return i;                   /* incomplete code set */
    }

    /* read in literal and distance code lengths */
    n = nl + nd;
    m = mask_bits[bl];
    i = l = 0;
    while ((unsigned)i < n) {
        NEEDBITS((unsigned)bl)
        j = (tl + ((unsigned)b & m))->v.n;
        DUMPBITS((tl + ((unsigned)b & m))->b)
        if (j < 16)                 /* length of code in bits (0..15) */
            ll[i++] = l = j;        /* save last length in l */
        else if (j == 16) {         /* repeat last length 3 to 6 times */
            NEEDBITS(2)
            j = 3 + ((unsigned)b & 3);
            DUMPBITS(2)
            if ((unsigned)i + j > n)
                return 1;
            while (j--)
                ll[i++] = l;
        }
        else if (j == 17) {         /* 3 to 10 zero length codes */
            NEEDBITS(3)
            j = 3 + ((unsigned)b & 7);
            DUMPBITS(3)
            if ((unsigned)i + j > n)
                return 1;
            while (j--)
                ll[i++] = 0;
            l = 0;
        }
        else {                      /* j == 18: 11 to 138 zero length codes */
            NEEDBITS(7)
            j = 11 + ((unsigned)b & 0x7f);
            DUMPBITS(7)
            if ((unsigned)i + j > n)
                return 1;
            while (j--)
                ll[i++] = 0;
            l = 0;
        }
    }

    /* free decoding table for trees */
    huft_free(tl);

    /* restore the global bit buffer */
    bb = b;
    bk = k;

    /* build the decoding tables for literal/length and distance codes */
    bl = lbits;
    if ((i = huft_build(ll, nl, 257, cplens, cplext, &tl, &bl)) != 0) {
        if (i == 1) {
            fprintf(stderr, " incomplete literal tree\n");
            huft_free(tl);
        }
        return i;                   /* incomplete code set */
    }
    bd = dbits;
    if ((i = huft_build(ll + nl, nd, 0, cpdist, cpdext, &td, &bd)) != 0) {
        if (i == 1) {
            fprintf(stderr, " incomplete distance tree\n");
            huft_free(td);
        }
        huft_free(tl);
        return i;                   /* incomplete code set */
    }

    /* decompress until an end-of-block code */
    if ((i = inflate_codes(tl, td, bl, bd)) != 0)
        return i;

    /* free the decoding tables, return */
    huft_free(tl);
    huft_free(td);
    return 0;
}

/* Decompress a block */
static int inflate_block(int *e)
{
    unsigned t;           /* block type */
    register ulg b;      /* bit buffer */
    register unsigned k;  /* number of bits in bit buffer */

    /* make local bit buffer */
    b = bb;
    k = bk;

    /* read in last block bit */
    NEEDBITS(1)
    *e = (int)b & 1;
    DUMPBITS(1)

    /* read in block type */
    NEEDBITS(2)
    t = (unsigned)b & 3;
    DUMPBITS(2)

    /* restore the global bit buffer */
    bb = b;
    bk = k;

    /* inflate that block type */
    if (t == 2)
        return inflate_dynamic();
    if (t == 0)
        return inflate_stored();
    if (t == 1)
        return inflate_fixed();

    /* bad block type */
    return 2;
}

/* Main inflate function */
static int inflate(void)
{
    int e;                /* last block flag */
    int r;                /* result code */
    unsigned h;          /* maximum struct huft's malloc'ed */

    /* initialize window, bit buffer */
    wp = 0;
    bk = 0;
    bb = 0;

    /* decompress until the last block */
    h = 0;
    do {
        debug_state("Starting new block");
        
        NEEDBITS(1)
        e = (int)b & 1;     /* NB: NEEDBITS() returns if not enough bits */
        DUMPBITS(1)

        NEEDBITS(2)
        r = (int)b & 3;
        DUMPBITS(2)

        debug_state("Block type");

        switch (r) {
        case 0:  /* stored */
            debug_state("Stored block");
            r = inflate_stored();
            break;
        case 1:  /* fixed Huffman */
            debug_state("Fixed Huffman block");
            r = inflate_fixed();
            break;
        case 2:  /* dynamic Huffman */
            debug_state("Dynamic Huffman block");
            r = inflate_dynamic();
            break;
        case 3:  /* illegal */
            debug_state("Invalid block type");
            r = 2;
            break;
        }

        if (r != 0) {
            fprintf(stderr, "Error processing block type %d (error: %d)\n", r, e);
            return r;
        }
    } while (!e);

    /* Flush out the window */
    flush_window();

    /* Return success */
    return 0;
}

/* Debug output function */
static void debug_state(const char *msg) {
    fprintf(stderr, "Debug: %s (inptr=%u, insize=%u, bk=%u, bb=0x%lx)\n", 
            msg, inptr, insize, bk, bb);
}

/* Main function for testing */
int main(int argc, char **argv)
{
    FILE *fp;
    size_t file_size;
    unsigned char magic[2];
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <compressed_file>\n", argv[0]);
        return 1;
    }

    /* Open input file */
    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror("Failed to open input file");
        return 1;
    }

    /* Check gzip magic header */
    if (fread(magic, 1, 2, fp) != 2 || magic[0] != 0x1f || magic[1] != 0x8b) {
        fprintf(stderr, "Error: Not a gzip file (magic=%02x %02x)\n", magic[0], magic[1]);
        fclose(fp);
        return 1;
    }

    /* Skip the rest of the gzip header (10 bytes total) */
    unsigned char header[8];
    if (fread(header, 1, 8, fp) != 8) {
        fprintf(stderr, "Error: Truncated gzip header\n");
        fclose(fp);
        return 1;
    }

    /* Get remaining file size */
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp) - 10;  /* Subtract header size */
    fseek(fp, 10, SEEK_SET);     /* Skip back to after header */

    debug_state("Before allocation");

    /* Allocate input buffer */
    inbuf = malloc(file_size);
    if (!inbuf) {
        perror("Failed to allocate input buffer");
        fclose(fp);
        return 1;
    }

    /* Read compressed data */
    if (fread(inbuf, 1, file_size, fp) != file_size) {
        perror("Failed to read input file");
        free(inbuf);
        fclose(fp);
        return 1;
    }

    fclose(fp);

    /* Initialize variables */
    insize = file_size;
    inptr = 0;
    bb = 0;
    bk = 0;
    wp = 0;

    debug_state("Before decompression");

    /* Decompress */
    int result = inflate();
    if (result != 0) {
        fprintf(stderr, "Error during decompression (code: %d)\n", result);
        free(inbuf);
        return 1;
    }

    debug_state("After decompression");

    /* Clean up */
    free(inbuf);
    return 0;
}
