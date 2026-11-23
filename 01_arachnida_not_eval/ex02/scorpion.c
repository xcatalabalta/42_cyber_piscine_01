/*
 * scorpion - display basic attributes and EXIF metadata of image files.
 *
 * Usage:   ./scorpion FILE1 [FILE2 ...]
 *
 * Handles .jpg/.jpeg, .png, .gif, .bmp. EXIF lives in JPEG; other formats
 * show basic attributes only. The EXIF/TIFF parsing is implemented here by
 * hand (byte scanning + manual endianness) -- no metadata library is used.
 *
 * Build:  make      (no external dependencies)
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

/* ---- EXIF tag id -> name tables ------------------------------------------ */
typedef struct { uint16_t id; const char *name; } tagname;

static const tagname TIFF_TAGS[] = {
    {0x0100,"ImageWidth"},{0x0101,"ImageLength"},{0x010E,"ImageDescription"},
    {0x010F,"Make"},{0x0110,"Model"},{0x0112,"Orientation"},
    {0x011A,"XResolution"},{0x011B,"YResolution"},{0x0128,"ResolutionUnit"},
    {0x0131,"Software"},{0x0132,"DateTime"},{0x013B,"Artist"},
    {0x8298,"Copyright"},{0x829A,"ExposureTime"},{0x829D,"FNumber"},
    {0x8822,"ExposureProgram"},{0x8827,"ISOSpeedRatings"},{0x9000,"ExifVersion"},
    {0x9003,"DateTimeOriginal"},{0x9004,"DateTimeDigitized"},
    {0x920A,"FocalLength"},{0xA002,"ExifImageWidth"},{0xA003,"ExifImageHeight"},
    {0xA430,"CameraOwnerName"},{0xA431,"BodySerialNumber"},{0,NULL}
};
static const tagname GPS_TAGS[] = {
    {0x0000,"GPSVersionID"},{0x0001,"GPSLatitudeRef"},{0x0002,"GPSLatitude"},
    {0x0003,"GPSLongitudeRef"},{0x0004,"GPSLongitude"},{0x0005,"GPSAltitudeRef"},
    {0x0006,"GPSAltitude"},{0,NULL}
};

static const char *name_of(const tagname *tbl, uint16_t id)
{
    static char hex[8];
    for (; tbl->name; tbl++)
        if (tbl->id == id)
            return tbl->name;
    snprintf(hex, sizeof(hex), "0x%04X", id);
    return hex;
}

/* field type -> element size in bytes (index by type code 1..12) */
static const int TYPE_SIZE[] = {0,1,1,2,4,8,1,1,2,4,8,4,8};

/* ---- byte-order aware readers (set per TIFF block) ----------------------- */
static int g_le = 1;                      /* 1 = little-endian TIFF */

static uint16_t rd16(const uint8_t *p)
{
    if (g_le)
        return (uint16_t)(p[0] | p[1] << 8);
    return (uint16_t)(p[1] | p[0] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    if (g_le)
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
               (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 |
           (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

/* ---- format detection + dimensions --------------------------------------- */
static const char *detect_format(const uint8_t *d, size_t n)
{
    if (n >= 3 && d[0]==0xFF && d[1]==0xD8 && d[2]==0xFF)
        return "JPEG";
    if (n >= 8 && !memcmp(d,"\x89PNG\r\n\x1a\n",8))
        return "PNG";
    if (n >= 6 && (!memcmp(d,"GIF87a",6)||!memcmp(d,"GIF89a",6)))
        return "GIF";
    if (n >= 2 && d[0]=='B' && d[1]=='M')
        return "BMP";
    return "UNKNOWN";
}

/* big-endian / little-endian helpers for the container headers */
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0]<<8 | p[1]);
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];
}
static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1]<<8);
}
static int32_t le32s(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24);
}

static int dimensions(const char *fmt, const uint8_t *d, size_t n,
                      uint32_t *w, uint32_t *h)
{
    if (!strcmp(fmt,"PNG") && n >= 24) {
        *w = be32(d+16);
        *h = be32(d+20);
        return 1;
    }
    if (!strcmp(fmt,"GIF") && n >= 10) {
        *w = le16(d+6);
        *h = le16(d+8);
        return 1;
    }
    if (!strcmp(fmt,"BMP") && n >= 26) {
        int32_t bw = le32s(d+18);
        int32_t bh = le32s(d+22);
        *w = (uint32_t)bw;
        if (bh < 0)
            *h = (uint32_t)(-bh);
        else
            *h = (uint32_t)bh;
        return 1;
    }
    if (!strcmp(fmt,"JPEG")) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t m = d[i+1];
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = be16(d+i+5);
                *w = be16(d+i+7);
                return 1;
            }
            if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7)) {
                i += 2;
                continue;
            }
            i += 2 + be16(d+i+2);
        }
    }
    return 0;
}

/* ---- EXIF ---------------------------------------------------------------- */
/* locate the APP1 Exif payload; return pointer to TIFF block + its length */
static const uint8_t *find_exif(const uint8_t *d, size_t n, size_t *tlen)
{
    size_t i = 2;
    while (i + 4 < n) {
        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        uint8_t m = d[i+1];
        if (m == 0xD9 || m == 0xDA)                   /* EOI / scan */
            break;
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            i += 2;
            continue;
        }
        uint16_t seg = be16(d+i+2);
        if (m == 0xE1 && i + 4 + 6 <= n &&
            !memcmp(d+i+4, "Exif\x00\x00", 6)) {
            *tlen = seg - 2 - 6;
            return d + i + 10;                        /* start of TIFF header */
        }
        i += 2 + seg;
    }
    return NULL;
}

/* print one entry's value, decoded by type */
static void print_value(const uint8_t *T, size_t tlen, uint16_t type,
                        uint32_t count, const uint8_t *vfield)
{
    if (type == 0 || type > 12) {
        printf("<type %u>", type);
        return;
    }
    uint32_t size = (uint32_t)TYPE_SIZE[type] * count;
    const uint8_t *data;
    if (size <= 4)
        data = vfield;
    else
        data = T + rd32(vfield);
    if (data < T || data + size > T + tlen) {
        printf("<out of range>");
        return;
    }

    switch (type) {
    case 2: {                                   /* ASCII */
        uint32_t k = 0;
        while (k < count && data[k] != '\0') {
            putchar(data[k]);
            k++;
        }
        break;
    }
    case 1:
    case 6:
    case 7:                                     /* BYTE / SBYTE / UNDEFINED */
        for (uint32_t k = 0; k < count && k < 32; k++) {
            if (k)
                printf(" ");
            printf("%u", data[k]);
        }
        break;
    case 3:
    case 8:                                     /* SHORT / SSHORT */
        for (uint32_t k = 0; k < count; k++) {
            if (k)
                printf(" ");
            printf("%u", rd16(data + k*2));
        }
        break;
    case 4:
    case 9:                                     /* LONG / SLONG */
        for (uint32_t k = 0; k < count; k++) {
            if (k)
                printf(" ");
            printf("%u", rd32(data + k*4));
        }
        break;
    case 5:
    case 10:                                    /* RATIONAL / SRATIONAL */
        for (uint32_t k = 0; k < count; k++) {
            uint32_t num = rd32(data + k*8);
            uint32_t den = rd32(data + k*8 + 4);
            if (k)
                printf(" ");
            printf("%u/%u", num, den);
        }
        break;
    default:
        printf("<%u bytes>", size);
    }
}

/* parse one IFD; capture Exif/GPS sub-IFD offsets via out-params */
static void parse_ifd(const uint8_t *T, size_t tlen, uint32_t off,
                      const tagname *names, uint32_t *exif_off, uint32_t *gps_off)
{
    if (off + 2 > tlen)
        return;
    uint16_t count = rd16(T + off);
    const uint8_t *p = T + off + 2;
    for (uint16_t e = 0; e < count; e++) {
        if ((size_t)(p - T) + 12 > tlen)
            break;
        uint16_t tag = rd16(p);
        uint16_t type = rd16(p + 2);
        uint32_t cnt = rd32(p + 4);
        const uint8_t *vfield = p + 8;
        p += 12;
        if (tag == 0x8769) {
            if (exif_off)
                *exif_off = rd32(vfield);
            continue;
        }
        if (tag == 0x8825) {
            if (gps_off)
                *gps_off = rd32(vfield);
            continue;
        }
        printf("      %-20s: ", name_of(names, tag));
        print_value(T, tlen, type, cnt, vfield);
        putchar('\n');
    }
}

static void parse_exif(const uint8_t *T, size_t tlen)
{
    if (tlen < 8) {
        printf("    (EXIF too short)\n");
        return;
    }
    if (!memcmp(T, "II", 2))
        g_le = 1;
    else if (!memcmp(T, "MM", 2))
        g_le = 0;
    else {
        printf("    (no TIFF byte-order mark)\n");
        return;
    }

    uint32_t ifd0 = rd32(T + 4);
    uint32_t exif_off = 0;
    uint32_t gps_off = 0;
    printf("    [IFD0 (main image)]\n");
    parse_ifd(T, tlen, ifd0, TIFF_TAGS, &exif_off, &gps_off);
    if (exif_off) {
        printf("    [Exif sub-IFD]\n");
        parse_ifd(T, tlen, exif_off, TIFF_TAGS, NULL, NULL);
    }
    if (gps_off) {
        printf("    [GPS IFD]\n");
        parse_ifd(T, tlen, gps_off, GPS_TAGS, NULL, NULL);
    }
}

/* ---- reporting ----------------------------------------------------------- */
static void report(const char *path)
{
    printf("============================================================\n");
    printf("%s\n", path);
    printf("============================================================\n");

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("  cannot stat file\n\n");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  cannot open file\n\n");
        return;
    }
    uint8_t *data = malloc(st.st_size);
    if (!data) {
        fclose(f);
        printf("  out of memory\n\n");
        return;
    }
    size_t n = fread(data, 1, st.st_size, f);
    fclose(f);

    const char *fmt = detect_format(data, n);
    printf("  Basic attributes:\n");
    printf("    Format        : %s\n", fmt);
    printf("    File size     : %lld bytes\n", (long long)st.st_size);
    uint32_t w;
    uint32_t h;
    if (dimensions(fmt, data, n, &w, &h))
        printf("    Dimensions    : %u x %u px\n", w, h);
    char buf[32];
    struct tm *tm = localtime(&st.st_mtime);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    printf("    Modified      : %s\n", buf);
    tm = localtime(&st.st_ctime);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    printf("    Inode changed : %s\n", buf);

    if (!strcmp(fmt, "JPEG")) {
        size_t tlen = 0;
        const uint8_t *T = find_exif(data, n, &tlen);
        if (!T)
            printf("\n  EXIF: none present\n");
        else {
            printf("\n  EXIF metadata:\n");
            parse_exif(T, tlen);
        }
    }
    else {
        printf("\n  EXIF: not applicable for this format\n");
    }
    printf("\n");
    free(data);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s FILE1 [FILE2 ...]\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++)
        report(argv[i]);
    return 0;
}
