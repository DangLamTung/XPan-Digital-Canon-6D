/**
 * film_xml.h - Dynamic XML Film Stock Parser & Directory Scanner for Magic Lantern
 *
 * Scans ML/FILMS/ on the SD card for user-defined or default film recipes in XML format.
 * Enables users to drop in custom film simulations, tweak Kelvin, WB Shifts, Picture Style
 * bases, and 3D LUT paths without recompiling the firmware module.
 */

#ifndef FILM_XML_H
#define FILM_XML_H

#include <stdint.h>
#include <string.h>
#include <fio-ml.h>
#include "../film_table.h"

/* The shelf can hold this many stocks: one entry per XML in ML/FILMS/.
 * It was 64 while the only definitions were the 33 compiled in; importing
 * M-Film's catalogue (tools/mfilm_import.py) took the card to 84, and going
 * past this limit drops the extras silently. film_active_table is one static
 * array, about 300 bytes per entry, so 96 costs ~29 KB of module BSS and
 * nothing per frame. */
#define MAX_DYNAMIC_FILMS 96
#define FILM_DIR "ML/FILMS"

typedef struct
{
    char name[32];          /* Display name (e.g. "Portra 400") */
    char code[12];          /* Short badge code (e.g. "P400") */
    char brand[16];         /* Brand ("Kodak", "Fujifilm", "Ilford") */
    char type[24];          /* Type ("Color Negative", "Slide", "B&W") */
    int base;               /* Canon Picture Style base ID (0x81..0x86) */
    int iso;                /* Rated box ISO (50..3200) */
    int raw_iso;            /* Canon raw ISO code (0x40..0x78) */
    int contrast;           /* Hardware contrast (-4..+4) */
    int sharpness;          /* Hardware sharpness (0..7) */
    int saturation;         /* Hardware saturation (-4..+4) */
    int tone;               /* Hardware colour tone (-4..+4) */
    int kelvin;             /* Color temperature (4000..6800 K) */
    int wbs_ba;             /* Blue/Amber WB Shift (-9..+9) */
    int wbs_gm;             /* Green/Magenta WB Shift (-9..+9) */
    int mono;               /* 1 = Monochrome (saturation/tone masked) */
    char lut[64];           /* Path to 3D LUT (.CUB) */
    char description[80];   /* Look description */
} film_xml_t;

static film_xml_t film_active_table[MAX_DYNAMIC_FILMS];
static int film_active_count = 0;

/* Lightweight zero-alloc XML tag parser */
static int fs_xml_tag(const char *xml, const char *tag, char *out, int max_len)
{
    char open_tag[32], close_tag[32];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return 0;
    start += strlen(open_tag);
    const char *end = strstr(start, close_tag);
    if (!end || end < start) return 0;

    int len = end - start;
    if (len >= max_len) len = max_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';

    /* Trim leading and trailing whitespace */
    int i = 0;
    while (out[i] == ' ' || out[i] == '\t' || out[i] == '\r' || out[i] == '\n') i++;
    if (i > 0) memmove(out, out + i, strlen(out + i) + 1);
    int l = strlen(out);
    while (l > 0 && (out[l-1] == ' ' || out[l-1] == '\t' || out[l-1] == '\r' || out[l-1] == '\n')) {
        out[--l] = '\0';
    }
    return 1;
}

static int fs_xml_int(const char *xml, const char *tag, int def_val)
{
    char buf[32];
    if (!fs_xml_tag(xml, tag, buf, sizeof(buf))) return def_val;
    if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
    {
        int val = 0;
        char *p = buf + 2;
        while (*p) {
            char c = *p++;
            int d = (c >= '0' && c <= '9') ? (c - '0') :
                    (c >= 'a' && c <= 'f') ? (c - 'a' + 10) :
                    (c >= 'A' && c <= 'F') ? (c - 'A' + 10) : -1;
            if (d < 0) break;
            val = (val << 4) | d;
        }
        return val;
    }
    int sign = 1, val = 0;
    char *p = buf;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return sign * val;
}

/* Fallback: populate active table from compiled-in film_table */
static void fs_populate_builtins(void)
{
    film_active_count = 0;
    for (int i = 0; i < FILM_COUNT && i < MAX_DYNAMIC_FILMS; i++)
    {
        const film_t *src = &film_table[i];
        film_xml_t *dst = &film_active_table[film_active_count++];
        snprintf(dst->name, sizeof(dst->name), "%s", src->name);
        snprintf(dst->code, sizeof(dst->code), "%s", src->code);
        snprintf(dst->brand, sizeof(dst->brand), "%s", src->brand);
        snprintf(dst->type, sizeof(dst->type), "%s", src->mono ? "Black & White" : "Color Negative");
        dst->base = src->base;
        dst->iso = src->iso;
        dst->raw_iso = src->raw_iso;
        dst->contrast = src->contrast;
        dst->sharpness = src->sharpness;
        dst->saturation = src->saturation;
        dst->tone = src->tone;
        dst->kelvin = src->kelvin;
        dst->wbs_ba = src->wbs_ba;
        dst->wbs_gm = src->wbs_gm;
        dst->mono = src->mono;
        snprintf(dst->lut, sizeof(dst->lut), "ML/DATA/%s.CUB", src->code);
        snprintf(dst->description, sizeof(dst->description), "%s", src->description);
    }
}

/* Save a film stock definition to an XML file */
static void fs_write_film_xml(const char *filename, const film_xml_t *f)
{
    FILE *fp = FIO_CreateFile(filename);
    if (!fp) return;

    char buf[512];
    snprintf(buf, sizeof(buf),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<film>\n"
        "    <name>%s</name>\n"
        "    <code>%s</code>\n"
        "    <brand>%s</brand>\n"
        "    <type>%s</type>\n"
        "    <base>0x%02x</base>\n"
        "    <iso>%d</iso>\n"
        "    <raw_iso>0x%02x</raw_iso>\n"
        "    <contrast>%+d</contrast>\n"
        "    <sharpness>%d</sharpness>\n"
        "    <saturation>%+d</saturation>\n"
        "    <tone>%+d</tone>\n"
        "    <kelvin>%d</kelvin>\n"
        "    <wbs_ba>%+d</wbs_ba>\n"
        "    <wbs_gm>%+d</wbs_gm>\n"
        "    <mono>%d</mono>\n"
        "    <lut>%s</lut>\n"
        "    <description>%s</description>\n"
        "</film>\n",
        f->name, f->code, f->brand, f->type, f->base, f->iso, f->raw_iso,
        f->contrast, f->sharpness, f->saturation, f->tone, f->kelvin,
        f->wbs_ba, f->wbs_gm, f->mono, f->lut, f->description);

    FIO_WriteFile(fp, buf, strlen(buf));
    FIO_CloseFile(fp);
}

/* Scan ML/FILMS/ and load all XML film stocks */
static int fs_load_films_xml(void)
{
    fs_populate_builtins();

    /* Ensure the directory exists ONCE. FIO_CreateDirectory() only returns
     * early while its is_dir() check succeeds; a transient failure makes it
     * create a duplicate FAT entry, and this ran on every catalogue reload. */
    {
        static int dir_ok;
        if (!dir_ok && FIO_CreateDirectory(FILM_DIR) == 0) dir_ok = 1;
    }

    struct fio_file *file = alloc_fio_file();
    if (!file) return film_active_count;

    struct fio_dirent * dirent = FIO_FindFirstEx(FILM_DIR, file);
    if (IS_ERROR(dirent))
    {
        free(file);
        /* Directory was missing or empty - export our 26 defaults to card */
        for (int i = 0; i < film_active_count; i++)
        {
            char path[80];
            snprintf(path, sizeof(path), "%s/%02d_%s.xml", FILM_DIR, i + 1, film_active_table[i].code);
            fs_write_film_xml(path, &film_active_table[i]);
        }
        return film_active_count;
    }

    /* Read user XML files from directory.  Parse straight into the active
     * table: a second MAX_DYNAMIC_FILMS staging array cost 17.9 KB of module
     * BSS and existed only to be memcpy'd once at the end. */
    int count = 0;

    do {
        struct file_info file_info = convert_fio_file_info(file);
        if (file_info.mode & ATTR_DIRECTORY) continue;
        int len = strlen(file_info.name);
        if (len < 5) continue;
        if (strcasecmp(file_info.name + len - 4, ".xml") != 0) continue;

        char fullpath[100];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", FILM_DIR, file_info.name);

        uint32_t size = 0;
        /* >= not >: size == 4096 passed, FIO_ReadFile filled xml_buf[0..4095],
         * and xml_buf[r] then wrote one byte past the end of the 4096-byte
         * static -- over whatever compiler put next to it. */
        static char xml_buf[4096];
        if (FIO_GetFileSize(fullpath, &size) != 0 || size == 0 ||
            size >= (uint32_t)sizeof xml_buf)
          continue;

        FILE *fp = FIO_OpenFile(fullpath, O_RDONLY | O_SYNC);
        if (!fp) continue;

        int r = FIO_ReadFile(fp, xml_buf, size);
        FIO_CloseFile(fp);
        if (r <= 0) continue;
        xml_buf[r] = '\0';

        film_xml_t *dst = &film_active_table[count];
        memset(dst, 0, sizeof(*dst));

        if (!fs_xml_tag(xml_buf, "name", dst->name, sizeof(dst->name))) continue;
        fs_xml_tag(xml_buf, "code", dst->code, sizeof(dst->code));
        fs_xml_tag(xml_buf, "brand", dst->brand, sizeof(dst->brand));
        fs_xml_tag(xml_buf, "type", dst->type, sizeof(dst->type));
        dst->base = fs_xml_int(xml_buf, "base", 0x84);
        dst->iso = fs_xml_int(xml_buf, "iso", 400);
        dst->raw_iso = fs_xml_int(xml_buf, "raw_iso", 0x58);
        dst->contrast = fs_xml_int(xml_buf, "contrast", 0);
        dst->sharpness = fs_xml_int(xml_buf, "sharpness", 2);
        dst->saturation = fs_xml_int(xml_buf, "saturation", 0);
        dst->tone = fs_xml_int(xml_buf, "tone", 0);
        dst->kelvin = fs_xml_int(xml_buf, "kelvin", 5200);
        dst->wbs_ba = fs_xml_int(xml_buf, "wbs_ba", 0);
        dst->wbs_gm = fs_xml_int(xml_buf, "wbs_gm", 0);
        dst->mono = fs_xml_int(xml_buf, "mono", 0);
        fs_xml_tag(xml_buf, "lut", dst->lut, sizeof(dst->lut));
        fs_xml_tag(xml_buf, "description", dst->description, sizeof(dst->description));

        count++;
        if (count >= MAX_DYNAMIC_FILMS) break;

    } while (FIO_FindNextEx(dirent, file) == 0);
    FIO_FindClose(dirent);
    free(file);

    if (count > 0)
        film_active_count = count;

    /*
     * Export any compiled-in stock that has no XML on the card yet.
     *
     * Builtins used to be exported only when ML/FILMS/ was completely empty,
     * so adding a stock to film_table.h did nothing on a card that already had
     * XML files - the new stock simply never appeared in the shelf. Codes are
     * unique per stock, so a missing code means a missing definition.
     */
    for (int i = 0; i < FILM_COUNT && film_active_count < MAX_DYNAMIC_FILMS; i++) {
        int have = 0;
        for (int k = 0; k < film_active_count; k++)
            if (!strcasecmp(film_active_table[k].code, film_table[i].code)) { have = 1; break; }
        if (have) continue;

        char path[80];
        snprintf(path, sizeof(path), "%s/%02d_%s.xml", FILM_DIR, i + 1, film_table[i].code);
        uint32_t probe = 0;
        if (FIO_GetFileSize(path, &probe) == 0) continue;    /* already there */

        film_xml_t *dst = &film_active_table[film_active_count];
        memset(dst, 0, sizeof(*dst));
        snprintf(dst->name, sizeof(dst->name), "%s", film_table[i].name);
        snprintf(dst->code, sizeof(dst->code), "%s", film_table[i].code);
        snprintf(dst->brand, sizeof(dst->brand), "%s", film_table[i].brand);
        snprintf(dst->type, sizeof(dst->type), "%s",
                 film_table[i].mono ? "Black & White" : "Color Negative");
        dst->base = film_table[i].base;
        dst->iso = film_table[i].iso;
        dst->raw_iso = film_table[i].raw_iso;
        dst->contrast = film_table[i].contrast;
        dst->sharpness = film_table[i].sharpness;
        dst->saturation = film_table[i].saturation;
        dst->tone = film_table[i].tone;
        dst->kelvin = film_table[i].kelvin;
        dst->wbs_ba = film_table[i].wbs_ba;
        dst->wbs_gm = film_table[i].wbs_gm;
        dst->mono = film_table[i].mono;
        snprintf(dst->description, sizeof(dst->description), "%s", film_table[i].description);
        fs_write_film_xml(path, dst);
        film_active_count++;
    }

    return film_active_count;
}

/* Resolve active dynamic stock to Picture Style hardware parameters */
static inline void film_active_to_style(int idx, int dev_mode, film_style_t * out)
{
    if (idx < 0 || idx >= film_active_count) idx = 0;
    const film_xml_t * f = &film_active_table[idx];

    out->base = f->base;
    out->sharpness  = f->sharpness;
    out->kelvin     = f->kelvin;
    out->wbs_ba     = f->wbs_ba;
    out->wbs_gm     = f->wbs_gm;

    /* Push / Pull chemistry modifiers */
    int c_boost = 0;
    int iso_offset = 0;
    switch (dev_mode)
    {
        case FS_DEV_PUSH_1:
            c_boost = 1;
            iso_offset = 8;     /* +1 EV */
            break;
        case FS_DEV_PUSH_2:
            c_boost = 2;
            iso_offset = 16;    /* +2 EV */
            break;
        case FS_DEV_PULL_1:
            c_boost = -1;
            iso_offset = -8;    /* -1 EV */
            break;
        default:
            break;
    }

    out->contrast = film_clamp(f->contrast + c_boost, -4, 4);
    out->raw_iso  = film_clamp(f->raw_iso + iso_offset, 0x48, 0x78);

    if (f->mono)
    {
        out->saturation = FILM_NA;
        out->tone       = FILM_NA;
    }
    else
    {
        out->saturation = film_clamp(f->saturation, -4, 4);
        out->tone       = film_clamp(f->tone, -4, 4);
    }
}

#endif /* FILM_XML_H */
