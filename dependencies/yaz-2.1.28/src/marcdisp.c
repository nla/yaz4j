/*
 * Copyright (C) 1995-2006, Index Data ApS
 * See the file LICENSE for details.
 *
 * $Id: marcdisp.c,v 1.34 2006/08/28 14:18:22 adam Exp $
 */

/**
 * \file marcdisp.c
 * \brief Implements MARC conversion utilities
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef WIN32
#include <windows.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <yaz/marcdisp.h>
#include <yaz/wrbuf.h>
#include <yaz/yaz-util.h>

#if YAZ_HAVE_XML2
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif

static void yaz_marc_reset(yaz_marc_t mt);

/** \brief node types for yaz_marc_node */
enum YAZ_MARC_NODE_TYPE
{ 
    YAZ_MARC_DATAFIELD,
    YAZ_MARC_CONTROLFIELD,
    YAZ_MARC_COMMENT,
    YAZ_MARC_LEADER
};

/** \brief represets a data field */
struct yaz_marc_datafield {
    char *tag;
    char *indicator;
    struct yaz_marc_subfield *subfields;
};

/** \brief represents a control field */
struct yaz_marc_controlfield {
    char *tag;
    char *data;
};

/** \brief a comment node */
struct yaz_marc_comment {
    char *comment;
};

/** \brief MARC node */
struct yaz_marc_node {
    enum YAZ_MARC_NODE_TYPE which;
    union {
        struct yaz_marc_datafield datafield;
        struct yaz_marc_controlfield controlfield;
        char *comment;
        char *leader;
    } u;
    struct yaz_marc_node *next;
};

/** \brief represents a subfield */
struct yaz_marc_subfield {
    char *code_data;
    struct yaz_marc_subfield *next;
};

/** \brief the internals of a yaz_marc_t handle */
struct yaz_marc_t_ {
    WRBUF m_wr;
    NMEM nmem;
    int xml;
    int debug;
    yaz_iconv_t iconv_cd;
    char subfield_str[8];
    char endline_str[8];
    char *leader_spec;
    struct yaz_marc_node *nodes;
    struct yaz_marc_node **nodes_pp;
    struct yaz_marc_subfield **subfield_pp;
};

yaz_marc_t yaz_marc_create(void)
{
    yaz_marc_t mt = (yaz_marc_t) xmalloc(sizeof(*mt));
    mt->xml = YAZ_MARC_LINE;
    mt->debug = 0;
    mt->m_wr = wrbuf_alloc();
    mt->iconv_cd = 0;
    mt->leader_spec = 0;
    strcpy(mt->subfield_str, " $");
    strcpy(mt->endline_str, "\n");

    mt->nmem = nmem_create();
    yaz_marc_reset(mt);
    return mt;
}

void yaz_marc_destroy(yaz_marc_t mt)
{
    if (!mt)
        return ;
    nmem_destroy(mt->nmem);
    wrbuf_free(mt->m_wr, 1);
    xfree(mt->leader_spec);
    xfree(mt);
}

static int marc_exec_leader(const char *leader_spec, char *leader,
                            size_t size);


struct yaz_marc_node *yaz_marc_add_node(yaz_marc_t mt)
{
    struct yaz_marc_node *n = nmem_malloc(mt->nmem, sizeof(*n));
    n->next = 0;
    *mt->nodes_pp = n;
    mt->nodes_pp = &n->next;
    return n;
}

void yaz_marc_add_comment(yaz_marc_t mt, char *comment)
{
    struct yaz_marc_node *n = yaz_marc_add_node(mt);
    n->which = YAZ_MARC_COMMENT;
    n->u.comment = nmem_strdup(mt->nmem, comment);
}

void yaz_marc_cprintf(yaz_marc_t mt, const char *fmt, ...)
{
    va_list ap;
    char buf[200];
    va_start(ap, fmt);

#ifdef WIN32
    _vsnprintf(buf, sizeof(buf)-1, fmt, ap);
#else
/* !WIN32 */
#if HAVE_VSNPRINTF
    vsnprintf(buf, sizeof(buf), fmt, ap);
#else
    vsprintf(buf, fmt, ap);
#endif
#endif
/* WIN32 */
    yaz_marc_add_comment(mt, buf);
    va_end (ap);
}

void yaz_marc_add_leader(yaz_marc_t mt, const char *leader, size_t leader_len)
{
    struct yaz_marc_node *n = yaz_marc_add_node(mt);
    n->which = YAZ_MARC_LEADER;
    n->u.leader = nmem_strdupn(mt->nmem, leader, leader_len);
    marc_exec_leader(mt->leader_spec, n->u.leader, leader_len);
}

void yaz_marc_add_controlfield(yaz_marc_t mt, const char *tag,
                               const char *data, size_t data_len)
{
    struct yaz_marc_node *n = yaz_marc_add_node(mt);
    n->which = YAZ_MARC_CONTROLFIELD;
    n->u.controlfield.tag = nmem_strdup(mt->nmem, tag);
    n->u.controlfield.data = nmem_strdupn(mt->nmem, data, data_len);
    if (mt->debug)
    {
        size_t i;
        char msg[80];

        sprintf(msg, "controlfield:");
        for (i = 0; i < 16 && i < data_len; i++)
            sprintf(msg + strlen(msg), " %02X", data[i] & 0xff);
        if (i < data_len)
            sprintf(msg + strlen(msg), " ..");
        yaz_marc_add_comment(mt, msg);
    }
}

#if YAZ_HAVE_XML2
void yaz_marc_add_controlfield_xml(yaz_marc_t mt, const xmlNode *ptr_tag,
                                   const xmlNode *ptr_data)
{
    struct yaz_marc_node *n = yaz_marc_add_node(mt);
    n->which = YAZ_MARC_CONTROLFIELD;
    n->u.controlfield.tag = nmem_text_node_cdata(ptr_tag, mt->nmem);
    n->u.controlfield.data = nmem_text_node_cdata(ptr_data, mt->nmem);
}
#endif

void yaz_marc_add_datafield(yaz_marc_t mt, const char *tag,
                            const char *indicator, size_t indicator_len)
{
    struct yaz_marc_node *n = yaz_marc_add_node(mt);
    n->which = YAZ_MARC_DATAFIELD;
    n->u.datafield.tag = nmem_strdup(mt->nmem, tag);
    n->u.datafield.indicator =
        nmem_strdupn(mt->nmem, indicator, indicator_len);
    n->u.datafield.subfields = 0;

    /* make subfield_pp the current (last one) */
    mt->subfield_pp = &n->u.datafield.subfields;
}

#if YAZ_HAVE_XML2
void yaz_marc_add_datafield_xml(yaz_marc_t mt, const xmlNode *ptr_tag,
                                const char *indicator, size_t indicator_len)
{
    struct yaz_marc_node *n = yaz_marc_add_node(mt);
    n->which = YAZ_MARC_DATAFIELD;
    n->u.datafield.tag = nmem_text_node_cdata(ptr_tag, mt->nmem);
    n->u.datafield.indicator =
        nmem_strdupn(mt->nmem, indicator, indicator_len);
    n->u.datafield.subfields = 0;

    /* make subfield_pp the current (last one) */
    mt->subfield_pp = &n->u.datafield.subfields;
}
#endif

void yaz_marc_add_subfield(yaz_marc_t mt,
                           const char *code_data, size_t code_data_len)
{
    if (mt->debug)
    {
        size_t i;
        char msg[80];

        sprintf(msg, "subfield:");
        for (i = 0; i < 16 && i < code_data_len; i++)
            sprintf(msg + strlen(msg), " %02X", code_data[i] & 0xff);
        if (i < code_data_len)
            sprintf(msg + strlen(msg), " ..");
        yaz_marc_add_comment(mt, msg);
    }

    if (mt->subfield_pp)
    {
        struct yaz_marc_subfield *n = nmem_malloc(mt->nmem, sizeof(*n));
        n->code_data = nmem_strdupn(mt->nmem, code_data, code_data_len);
        n->next = 0;
        /* mark subfield_pp to point to this one, so we append here next */
        *mt->subfield_pp = n;
        mt->subfield_pp = &n->next;
    }
}

static int atoi_n_check(const char *buf, int size, int *val)
{
    if (!isdigit(*(const unsigned char *) buf))
        return 0;
    *val = atoi_n(buf, size);
    return 1;
}

/** \brief reads the MARC 24 bytes leader and checks content
    \param mt handle
    \param leader of the 24 byte leader
    \param indicator_length indicator length
    \param identifier_length identifier length
    \param base_address base address
    \param length_data_entry length of data entry
    \param length_starting length of starting 
    \param length_implementation length of implementation defined data
*/
static void yaz_marc_read_leader(yaz_marc_t mt, const char *leader_c,
                                 int *indicator_length,
                                 int *identifier_length,
                                 int *base_address,
                                 int *length_data_entry,
                                 int *length_starting,
                                 int *length_implementation)
{
    char leader[24];

    memcpy(leader, leader_c, 24);

    if (!atoi_n_check(leader+10, 1, indicator_length))
    {
        yaz_marc_cprintf(mt, 
                         "Indicator length at offset 10 should hold a digit."
                         " Assuming 2");
        leader[10] = '2';
        *indicator_length = 2;
    }
    if (!atoi_n_check(leader+11, 1, identifier_length))
    {
        yaz_marc_cprintf(mt, 
                         "Identifier length at offset 11 should hold a digit."
                         " Assuming 2");
        leader[11] = '2';
        *identifier_length = 2;
    }
    if (!atoi_n_check(leader+12, 5, base_address))
    {
        yaz_marc_cprintf(mt, 
                         "Base address at offsets 12..16 should hold a number."
                         " Assuming 0");
        *base_address = 0;
    }
    if (!atoi_n_check(leader+20, 1, length_data_entry))
    {
        yaz_marc_cprintf(mt, 
                         "Length data entry at offset 20 should hold a digit."
                         " Assuming 4");
        *length_data_entry = 4;
        leader[20] = '4';
    }
    if (!atoi_n_check(leader+21, 1, length_starting))
    {
        yaz_marc_cprintf(mt,
                         "Length starting at offset 21 should hold a digit."
                         " Assuming 5");
        *length_starting = 5;
        leader[21] = '5';
    }
    if (!atoi_n_check(leader+22, 1, length_implementation))
    {
        yaz_marc_cprintf(mt, 
                         "Length implementation at offset 22 should hold a digit."
                         " Assuming 0");
        *length_implementation = 0;
        leader[22] = '0';
    }

    if (mt->debug)
    {
        yaz_marc_cprintf(mt, "Indicator length      %5d", *indicator_length);
        yaz_marc_cprintf(mt, "Identifier length     %5d", *identifier_length);
        yaz_marc_cprintf(mt, "Base address          %5d", *base_address);
        yaz_marc_cprintf(mt, "Length data entry     %5d", *length_data_entry);
        yaz_marc_cprintf(mt, "Length starting       %5d", *length_starting);
        yaz_marc_cprintf(mt, "Length implementation %5d", *length_implementation);
    }
    yaz_marc_add_leader(mt, leader, 24);
}

void yaz_marc_subfield_str(yaz_marc_t mt, const char *s)
{
    strncpy(mt->subfield_str, s, sizeof(mt->subfield_str)-1);
    mt->subfield_str[sizeof(mt->subfield_str)-1] = '\0';
}

void yaz_marc_endline_str(yaz_marc_t mt, const char *s)
{
    strncpy(mt->endline_str, s, sizeof(mt->endline_str)-1);
    mt->endline_str[sizeof(mt->endline_str)-1] = '\0';
}

/* try to guess how many bytes the identifier really is! */
static size_t cdata_one_character(yaz_marc_t mt, const char *buf)
{
    if (mt->iconv_cd)
    {
        size_t i;
        for (i = 1; i<5; i++)
        {
            char outbuf[12];
            size_t outbytesleft = sizeof(outbuf);
            char *outp = outbuf;
            const char *inp = buf;

            size_t inbytesleft = i;
            size_t r = yaz_iconv(mt->iconv_cd, (char**) &inp, &inbytesleft,
                                 &outp, &outbytesleft);
            if (r != (size_t) (-1))
                return i;  /* got a complete sequence */
        }
        return 1; /* giving up */
    }
    return 1; /* we don't know */
}
                              
static void yaz_marc_reset(yaz_marc_t mt)
{
    nmem_reset(mt->nmem);
    mt->nodes = 0;
    mt->nodes_pp = &mt->nodes;
    mt->subfield_pp = 0;
}

int yaz_marc_write_line(yaz_marc_t mt, WRBUF wr)
{
    struct yaz_marc_node *n;
    int identifier_length;
    const char *leader = 0;

    for (n = mt->nodes; n; n = n->next)
        if (n->which == YAZ_MARC_LEADER)
        {
            leader = n->u.leader;
            break;
        }
    
    if (!leader)
        return -1;
    if (!atoi_n_check(leader+11, 1, &identifier_length))
        return -1;

    for (n = mt->nodes; n; n = n->next)
    {
        struct yaz_marc_subfield *s;
        switch(n->which)
        {
        case YAZ_MARC_DATAFIELD:
            wrbuf_printf(wr, "%s %s", n->u.datafield.tag,
                         n->u.datafield.indicator);
            for (s = n->u.datafield.subfields; s; s = s->next)
            {
                /* if identifier length is 2 (most MARCs),
                   the code is a single character .. However we've
                   seen multibyte codes, so see how big it really is */
                size_t using_code_len = 
                    (identifier_length != 2) ? identifier_length - 1
                    :
                    cdata_one_character(mt, s->code_data);
                
                wrbuf_puts (wr, mt->subfield_str); 
                wrbuf_iconv_write(wr, mt->iconv_cd, s->code_data, 
                                  using_code_len);
                wrbuf_iconv_puts(wr, mt->iconv_cd, " ");
                wrbuf_iconv_puts(wr, mt->iconv_cd, 
                                 s->code_data + using_code_len);
                wrbuf_iconv_puts(wr, mt->iconv_cd, " ");
                wr->pos--;
            }
            wrbuf_puts (wr, mt->endline_str);
            break;
        case YAZ_MARC_CONTROLFIELD:
            wrbuf_printf(wr, "%s", n->u.controlfield.tag);
            wrbuf_iconv_puts(wr, mt->iconv_cd, " ");
            wrbuf_iconv_puts(wr, mt->iconv_cd, n->u.controlfield.data);
            wrbuf_iconv_puts(wr, mt->iconv_cd, " ");
            wr->pos--;
            wrbuf_puts (wr, mt->endline_str);
            break;
        case YAZ_MARC_COMMENT:
            wrbuf_puts(wr, "(");
            wrbuf_iconv_write(wr, mt->iconv_cd, 
                              n->u.comment, strlen(n->u.comment));
            wrbuf_puts(wr, ")\n");
            break;
        case YAZ_MARC_LEADER:
            wrbuf_printf(wr, "%s\n", n->u.leader);
        }
    }
    return 0;
}

int yaz_marc_write_mode(yaz_marc_t mt, WRBUF wr)
{
    switch(mt->xml)
    {
    case YAZ_MARC_LINE:
        return yaz_marc_write_line(mt, wr);
    case YAZ_MARC_MARCXML:
        return yaz_marc_write_marcxml(mt, wr);
    case YAZ_MARC_XCHANGE:
        return yaz_marc_write_marcxchange(mt, wr, 0, 0); /* no format, type */
    case YAZ_MARC_ISO2709:
        return yaz_marc_write_iso2709(mt, wr);
    }
    return -1;
}

/** \brief common MARC XML/Xchange writer
    \param mt handle
    \param wr WRBUF output
    \param ns XMLNS for the elements
    \param format record format (e.g. "MARC21")
    \param type record type (e.g. "Bibliographic")
*/
static int yaz_marc_write_marcxml_ns(yaz_marc_t mt, WRBUF wr,
                                     const char *ns, 
                                     const char *format,
                                     const char *type)
{
    struct yaz_marc_node *n;
    int identifier_length;
    const char *leader = 0;

    for (n = mt->nodes; n; n = n->next)
        if (n->which == YAZ_MARC_LEADER)
        {
            leader = n->u.leader;
            break;
        }
    
    if (!leader)
        return -1;
    if (!atoi_n_check(leader+11, 1, &identifier_length))
        return -1;

    wrbuf_printf(wr, "<record xmlns=\"%s\"", ns);
    if (format)
        wrbuf_printf(wr, " format=\"%.80s\"", format);
    if (type)
        wrbuf_printf(wr, " type=\"%.80s\"", type);
    wrbuf_printf(wr, ">\n");
    for (n = mt->nodes; n; n = n->next)
    {
        struct yaz_marc_subfield *s;

        switch(n->which)
        {
        case YAZ_MARC_DATAFIELD:
            wrbuf_printf(wr, "  <datafield tag=\"");
            wrbuf_iconv_write_cdata(wr, mt->iconv_cd, n->u.datafield.tag,
                                    strlen(n->u.datafield.tag));
            wrbuf_printf(wr, "\"");
            if (n->u.datafield.indicator)
            {
                int i;
                for (i = 0; n->u.datafield.indicator[i]; i++)
                {
                    wrbuf_printf(wr, " ind%d=\"", i+1);
                    wrbuf_iconv_write_cdata(wr, mt->iconv_cd,
                                          n->u.datafield.indicator+i, 1);
                    wrbuf_iconv_puts(wr, mt->iconv_cd, "\"");
                }
            }
            wrbuf_printf(wr, ">\n");
            for (s = n->u.datafield.subfields; s; s = s->next)
            {
                /* if identifier length is 2 (most MARCs),
                   the code is a single character .. However we've
                   seen multibyte codes, so see how big it really is */
                size_t using_code_len = 
                    (identifier_length != 2) ? identifier_length - 1
                    :
                    cdata_one_character(mt, s->code_data);
                
                wrbuf_iconv_puts(wr, mt->iconv_cd, "    <subfield code=\"");
                wrbuf_iconv_write_cdata(wr, mt->iconv_cd,
                                        s->code_data, using_code_len);
                wrbuf_iconv_puts(wr, mt->iconv_cd, "\">");
                wrbuf_iconv_write_cdata(wr, mt->iconv_cd,
                                        s->code_data + using_code_len,
                                        strlen(s->code_data + using_code_len));
                wrbuf_iconv_puts(wr, mt->iconv_cd, "</subfield>");
                wrbuf_puts(wr, "\n");
            }
            wrbuf_printf(wr, "  </datafield>\n");
            break;
        case YAZ_MARC_CONTROLFIELD:
            wrbuf_printf(wr, "  <controlfield tag=\"");
            wrbuf_iconv_write_cdata(wr, mt->iconv_cd, n->u.controlfield.tag,
                                    strlen(n->u.controlfield.tag));
            wrbuf_iconv_puts(wr, mt->iconv_cd, "\">");
            wrbuf_iconv_puts(wr, mt->iconv_cd, n->u.controlfield.data);
            wrbuf_iconv_puts(wr, mt->iconv_cd, "</controlfield>");
            wrbuf_puts(wr, "\n");
            break;
        case YAZ_MARC_COMMENT:
            wrbuf_printf(wr, "<!-- ");
            wrbuf_puts(wr, n->u.comment);
            wrbuf_printf(wr, " -->\n");
            break;
        case YAZ_MARC_LEADER:
            wrbuf_printf(wr, "  <leader>");
            wrbuf_iconv_write_cdata(wr, 
                                    0 /* no charset conversion for leader */,
                                    n->u.leader, strlen(n->u.leader));
            wrbuf_printf(wr, "</leader>\n");
        }
    }
    wrbuf_puts(wr, "</record>\n");
    return 0;
}

int yaz_marc_write_marcxml(yaz_marc_t mt, WRBUF wr)
{
    if (!mt->leader_spec)
        yaz_marc_modify_leader(mt, 9, "a");
    return yaz_marc_write_marcxml_ns(mt, wr, "http://www.loc.gov/MARC21/slim",
                                     0, 0);
}

int yaz_marc_write_marcxchange(yaz_marc_t mt, WRBUF wr,
                               const char *format,
                               const char *type)
{
    return yaz_marc_write_marcxml_ns(mt, wr,
                                     "http://www.bs.dk/standards/MarcXchange",
                                     0, 0);
}

int yaz_marc_write_iso2709(yaz_marc_t mt, WRBUF wr)
{
    struct yaz_marc_node *n;
    int indicator_length;
    int identifier_length;
    int length_data_entry;
    int length_starting;
    int length_implementation;
    int data_offset = 0;
    const char *leader = 0;
    WRBUF wr_dir, wr_head, wr_data_tmp;
    int base_address;
    
    for (n = mt->nodes; n; n = n->next)
        if (n->which == YAZ_MARC_LEADER)
            leader = n->u.leader;
    
    if (!leader)
        return -1;
    if (!atoi_n_check(leader+10, 1, &indicator_length))
        return -1;
    if (!atoi_n_check(leader+11, 1, &identifier_length))
        return -1;
    if (!atoi_n_check(leader+20, 1, &length_data_entry))
        return -1;
    if (!atoi_n_check(leader+21, 1, &length_starting))
        return -1;
    if (!atoi_n_check(leader+22, 1, &length_implementation))
        return -1;

    wr_data_tmp = wrbuf_alloc();
    wr_dir = wrbuf_alloc();
    for (n = mt->nodes; n; n = n->next)
    {
        int data_length = 0;
        struct yaz_marc_subfield *s;

        switch(n->which)
        {
        case YAZ_MARC_DATAFIELD:
            wrbuf_printf(wr_dir, "%.3s", n->u.datafield.tag);
            data_length += indicator_length;
            wrbuf_rewind(wr_data_tmp);
            for (s = n->u.datafield.subfields; s; s = s->next)
            {
                /* write dummy IDFS + content */
                wrbuf_iconv_putchar(wr_data_tmp, mt->iconv_cd, ' ');
                wrbuf_iconv_puts(wr_data_tmp, mt->iconv_cd, s->code_data);
            }
            /* write dummy FS (makes MARC-8 to become ASCII) */
            wrbuf_iconv_putchar(wr_data_tmp, mt->iconv_cd, ' ');
            data_length += wrbuf_len(wr_data_tmp);
            break;
        case YAZ_MARC_CONTROLFIELD:
            wrbuf_printf(wr_dir, "%.3s", n->u.controlfield.tag);

            wrbuf_rewind(wr_data_tmp);
            wrbuf_iconv_puts(wr_data_tmp, mt->iconv_cd, 
                             n->u.controlfield.data);
            wrbuf_iconv_putchar(wr_data_tmp, mt->iconv_cd, ' ');/* field sep */
            data_length += wrbuf_len(wr_data_tmp);
            break;
        case YAZ_MARC_COMMENT:
            break;
        case YAZ_MARC_LEADER:
            break;
        }
        if (data_length)
        {
            wrbuf_printf(wr_dir, "%0*d", length_data_entry, data_length);
            wrbuf_printf(wr_dir, "%0*d", length_starting, data_offset);
            data_offset += data_length;
        }
    }
    /* mark end of directory */
    wrbuf_putc(wr_dir, ISO2709_FS);

    /* base address of data (comes after leader+directory) */
    base_address = 24 + wrbuf_len(wr_dir);

    wr_head = wrbuf_alloc();

    /* write record length */
    wrbuf_printf(wr_head, "%05d", base_address + data_offset + 1);
    /* from "original" leader */
    wrbuf_write(wr_head, leader+5, 7);
    /* base address of data */
    wrbuf_printf(wr_head, "%05d", base_address);
    /* from "original" leader */
    wrbuf_write(wr_head, leader+17, 7);
    
    wrbuf_write(wr, wrbuf_buf(wr_head), 24);
    wrbuf_write(wr, wrbuf_buf(wr_dir), wrbuf_len(wr_dir));
    wrbuf_free(wr_head, 1);
    wrbuf_free(wr_dir, 1);
    wrbuf_free(wr_data_tmp, 1);

    for (n = mt->nodes; n; n = n->next)
    {
        struct yaz_marc_subfield *s;

        switch(n->which)
        {
        case YAZ_MARC_DATAFIELD:
            wrbuf_printf(wr, "%.*s", indicator_length,
                         n->u.datafield.indicator);
            for (s = n->u.datafield.subfields; s; s = s->next)
            {
                wrbuf_putc(wr, ISO2709_IDFS);
                wrbuf_iconv_puts(wr, mt->iconv_cd, s->code_data);
                /* write dummy blank - makes MARC-8 to become ASCII */
                wrbuf_iconv_putchar(wr, mt->iconv_cd, ' ');
                wr->pos--;
            }
            wrbuf_putc(wr, ISO2709_FS);
            break;
        case YAZ_MARC_CONTROLFIELD:
            wrbuf_iconv_puts(wr, mt->iconv_cd, n->u.controlfield.data);
            /* write dummy blank - makes MARC-8 to become ASCII */
            wrbuf_iconv_putchar(wr, mt->iconv_cd, ' ');
            wr->pos--;
            wrbuf_putc(wr, ISO2709_FS);
            break;
        case YAZ_MARC_COMMENT:
            break;
        case YAZ_MARC_LEADER:
            break;
        }
    }
    wrbuf_printf(wr, "%c", ISO2709_RS);
    return 0;
}

#if YAZ_HAVE_XML2
int yaz_marc_read_xml_subfields(yaz_marc_t mt, const xmlNode *ptr)
{
    for (; ptr; ptr = ptr->next)
    {
        if (ptr->type == XML_ELEMENT_NODE)
        {
            if (!strcmp((const char *) ptr->name, "subfield"))
            {
                size_t ctrl_data_len = 0;
                char *ctrl_data_buf = 0;
                const xmlNode *p = 0, *ptr_code = 0;
                struct _xmlAttr *attr;
                for (attr = ptr->properties; attr; attr = attr->next)
                    if (!strcmp((const char *)attr->name, "code"))
                        ptr_code = attr->children;
                    else
                    {
                        yaz_marc_cprintf(
                            mt, "Bad attribute '%.80s' for 'subfield'",
                            attr->name);
                        return -1;
                    }
                if (!ptr_code)
                {
                    yaz_marc_cprintf(
                        mt, "Missing attribute 'code' for 'subfield'" );
                    return -1;
                }
                if (ptr_code->type == XML_TEXT_NODE)
                {
                    ctrl_data_len = 
                        strlen((const char *)ptr_code->content);
                }
                else
                {
                    yaz_marc_cprintf(
                        mt, "Missing value for 'code' in 'subfield'" );
                    return -1;
                }
                for (p = ptr->children; p ; p = p->next)
                    if (p->type == XML_TEXT_NODE)
                        ctrl_data_len += strlen((const char *)p->content);
                ctrl_data_buf = nmem_malloc(mt->nmem, ctrl_data_len+1);
                strcpy(ctrl_data_buf, (const char *)ptr_code->content);
                for (p = ptr->children; p ; p = p->next)
                    if (p->type == XML_TEXT_NODE)
                        strcat(ctrl_data_buf, (const char *)p->content);
                yaz_marc_add_subfield(mt, ctrl_data_buf, ctrl_data_len);
            }
            else
            {
                yaz_marc_cprintf(
                    mt, "Expected element 'subfield', got '%.80s'", ptr->name);
                return -1;
            }
        }
    }
    return 0;
}

static int yaz_marc_read_xml_leader(yaz_marc_t mt, const xmlNode **ptr_p)
{
    int indicator_length;
    int identifier_length;
    int base_address;
    int length_data_entry;
    int length_starting;
    int length_implementation;
    const char *leader = 0;
    const xmlNode *ptr = *ptr_p;

    for(; ptr; ptr = ptr->next)
        if (ptr->type == XML_ELEMENT_NODE)
        {
            if (!strcmp((const char *) ptr->name, "leader"))
            {
                xmlNode *p = ptr->children;
                for(; p; p = p->next)
                    if (p->type == XML_TEXT_NODE)
                        leader = (const char *) p->content;
                break;
            }
            else
            {
                yaz_marc_cprintf(
                    mt, "Expected element 'leader', got '%.80s'", ptr->name);
                return -1;
            }
        }
    if (!leader)
    {
        yaz_marc_cprintf(mt, "Missing element 'leader'");
        return -1;
    }
    if (strlen(leader) != 24)
    {
        yaz_marc_cprintf(mt, "Bad length %d of leader data."
                         " Must have length of 24 characters", strlen(leader));
        return -1;
    }
    yaz_marc_read_leader(mt, leader,
                         &indicator_length,
                         &identifier_length,
                         &base_address,
                         &length_data_entry,
                         &length_starting,
                         &length_implementation);
    *ptr_p = ptr;
    return 0;
}

static int yaz_marc_read_xml_fields(yaz_marc_t mt, const xmlNode *ptr)
{
    for(; ptr; ptr = ptr->next)
        if (ptr->type == XML_ELEMENT_NODE)
        {
            if (!strcmp((const char *) ptr->name, "controlfield"))
            {
                const xmlNode *ptr_tag = 0;
                struct _xmlAttr *attr;
                for (attr = ptr->properties; attr; attr = attr->next)
                    if (!strcmp((const char *)attr->name, "tag"))
                        ptr_tag = attr->children;
                    else
                    {
                        yaz_marc_cprintf(
                            mt, "Bad attribute '%.80s' for 'controlfield'",
                            attr->name);
                        return -1;
                    }
                if (!ptr_tag)
                {
                    yaz_marc_cprintf(
                        mt, "Missing attribute 'tag' for 'controlfield'" );
                    return -1;
                }
                yaz_marc_add_controlfield_xml(mt, ptr_tag, ptr->children);
            }
            else if (!strcmp((const char *) ptr->name, "datafield"))
            {
                char indstr[11]; /* 0(unused), 1,....9, + zero term */
                const xmlNode *ptr_tag = 0;
                struct _xmlAttr *attr;
                int i;
                for (i = 0; i<11; i++)
                    indstr[i] = '\0';
                for (attr = ptr->properties; attr; attr = attr->next)
                    if (!strcmp((const char *)attr->name, "tag"))
                        ptr_tag = attr->children;
                    else if (strlen((const char *)attr->name) == 4 &&
                             !memcmp(attr->name, "ind", 3))
                    {
                        int no = atoi((const char *)attr->name+3);
                        if (attr->children
                            && attr->children->type == XML_TEXT_NODE)
                            indstr[no] = attr->children->content[0];
                    }
                    else
                    {
                        yaz_marc_cprintf(
                            mt, "Bad attribute '%.80s' for 'datafield'",
                            attr->name);
                        return -1;
                    }
                if (!ptr_tag)
                {
                    yaz_marc_cprintf(
                        mt, "Missing attribute 'tag' for 'datafield'" );
                    return -1;
                }
                /* note that indstr[0] is unused so we use indstr[1..] */
                yaz_marc_add_datafield_xml(mt, ptr_tag,
                                           indstr+1, strlen(indstr+1));
                
                if (yaz_marc_read_xml_subfields(mt, ptr->children))
                    return -1;
            }
            else
            {
                yaz_marc_cprintf(mt,
                                 "Expected element controlfield or datafield,"
                                 " got %.80s", ptr->name);
                return -1;
            }
        }
    return 0;
}

int yaz_marc_read_xml(yaz_marc_t mt, const void *xmlnode)
{
    const xmlNode *ptr = xmlnode;
    for(; ptr; ptr = ptr->next)
        if (ptr->type == XML_ELEMENT_NODE)
        {
            if (!strcmp((const char *) ptr->name, "record"))
                break;
            else
            {
                yaz_marc_cprintf(
                    mt, "Unknown element '%.80s' in MARC XML reader",
                    ptr->name);
                return -1;
            }
        }
    if (!ptr)
    {
        yaz_marc_cprintf(mt, "Missing element 'record' in MARC XML record");
        return -1;
    }
    /* ptr points to record node now */
    ptr = ptr->children;
    if (yaz_marc_read_xml_leader(mt, &ptr))
        return -1;
    return yaz_marc_read_xml_fields(mt, ptr->next);
}
#else
int yaz_marc_read_xml(yaz_marc_t mt, const void *xmlnode)
{
    return -1;
}
#endif

int yaz_marc_read_iso2709(yaz_marc_t mt, const char *buf, int bsize)
{
    int entry_p;
    int record_length;
    int indicator_length;
    int identifier_length;
    int end_of_directory;
    int base_address;
    int length_data_entry;
    int length_starting;
    int length_implementation;

    yaz_marc_reset(mt);

    record_length = atoi_n (buf, 5);
    if (record_length < 25)
    {
        yaz_marc_cprintf(mt, "Record length %d < 24", record_length);
        return -1;
    }
    /* ballout if bsize is known and record_length is less than that */
    if (bsize != -1 && record_length > bsize)
    {
        yaz_marc_cprintf(mt, "Record appears to be larger than buffer %d < %d",
                         record_length, bsize);
        return -1;
    }
    if (mt->debug)
        yaz_marc_cprintf(mt, "Record length         %5d", record_length);

    yaz_marc_read_leader(mt, buf,
                         &indicator_length,
                         &identifier_length,
                         &base_address,
                         &length_data_entry,
                         &length_starting,
                         &length_implementation);

    /* First pass. determine length of directory & base of data */
    for (entry_p = 24; buf[entry_p] != ISO2709_FS; )
    {
        /* length of directory entry */
        int l = 3 + length_data_entry + length_starting;
        if (entry_p + l >= record_length)
        {
            yaz_marc_cprintf(mt, "Directory offset %d: end of record."
                             " Missing FS char", entry_p);
            return -1;
        }
        if (mt->debug)
        {
            yaz_marc_cprintf(mt, "Directory offset %d: Tag %.3s",
                             entry_p, buf+entry_p);
        }
        /* Check for digits in length info */
        while (--l >= 3)
            if (!isdigit(*(const unsigned char *) (buf + entry_p+l)))
                break;
        if (l >= 3)
        {
            /* Not all digits, so stop directory scan */
            yaz_marc_cprintf(mt, "Directory offset %d: Bad value for data"
                             " length and/or length starting", entry_p);
            break;
        }
        entry_p += 3 + length_data_entry + length_starting;
    }
    end_of_directory = entry_p;
    if (base_address != entry_p+1)
    {
        yaz_marc_cprintf(mt, "Base address not at end of directory,"
                         " base %d, end %d", base_address, entry_p+1);
    }

    /* Second pass. parse control - and datafields */
    for (entry_p = 24; entry_p != end_of_directory; )
    {
        int data_length;
        int data_offset;
        int end_offset;
        int i;
        char tag[4];
        int identifier_flag = 0;
        int entry_p0 = entry_p;

        memcpy (tag, buf+entry_p, 3);
        entry_p += 3;
        tag[3] = '\0';
        data_length = atoi_n(buf+entry_p, length_data_entry);
        entry_p += length_data_entry;
        data_offset = atoi_n(buf+entry_p, length_starting);
        entry_p += length_starting;
        i = data_offset + base_address;
        end_offset = i+data_length-1;

        if (data_length <= 0 || data_offset < 0)
            break;
        
        if (mt->debug)
        {
            yaz_marc_cprintf(mt, "Tag: %s. Directory offset %d: data-length %d,"
                             " data-offset %d",
                             tag, entry_p0, data_length, data_offset);
        }
        if (end_offset >= record_length)
        {
            yaz_marc_cprintf(mt, "Directory offset %d: Data out of bounds %d >= %d",
                             entry_p0, end_offset, record_length);
            break;
        }
        
        if (memcmp (tag, "00", 2))
            identifier_flag = 1;  /* if not 00X assume subfields */
        else if (indicator_length < 4 && indicator_length > 0)
        {
            /* Danmarc 00X have subfields */
            if (buf[i + indicator_length] == ISO2709_IDFS)
                identifier_flag = 1;
            else if (buf[i + indicator_length + 1] == ISO2709_IDFS)
                identifier_flag = 2;
        }

        if (identifier_flag)
        {
            /* datafield */
            i += identifier_flag-1;
            yaz_marc_add_datafield(mt, tag, buf+i, indicator_length);
            i += indicator_length;

            while (i < end_offset &&
                    buf[i] != ISO2709_RS && buf[i] != ISO2709_FS)
            {
                int code_offset = i+1;

                i ++;
                while (i < end_offset &&
                        buf[i] != ISO2709_RS && buf[i] != ISO2709_IDFS &&
                       buf[i] != ISO2709_FS)
                    i++;
                yaz_marc_add_subfield(mt, buf+code_offset, i - code_offset);
            }
        }
        else
        {
            /* controlfield */
            int i0 = i;
            while (i < end_offset && 
                buf[i] != ISO2709_RS && buf[i] != ISO2709_FS)
                i++;
            yaz_marc_add_controlfield(mt, tag, buf+i0, i-i0);
        }
        if (i < end_offset)
        {
            yaz_marc_cprintf(mt, "Separator but not at end of field length=%d",
                    data_length);
        }
        if (buf[i] != ISO2709_RS && buf[i] != ISO2709_FS)
        {
            yaz_marc_cprintf(mt, "No separator at end of field length=%d",
                    data_length);
        }
    }
    return record_length;
}

int yaz_marc_decode_wrbuf(yaz_marc_t mt, const char *buf, int bsize, WRBUF wr)
{
    int s, r = yaz_marc_read_iso2709(mt, buf, bsize);
    if (r <= 0)
        return r;
    s = yaz_marc_write_mode(mt, wr); /* returns 0 for OK, -1 otherwise */
    if (s != 0)
        return -1; /* error */
    return r; /* OK, return length > 0 */
}

int yaz_marc_decode_buf (yaz_marc_t mt, const char *buf, int bsize,
                         char **result, int *rsize)
{
    int r;

    wrbuf_rewind(mt->m_wr);
    r = yaz_marc_decode_wrbuf(mt, buf, bsize, mt->m_wr);
    if (result)
        *result = wrbuf_buf(mt->m_wr);
    if (rsize)
        *rsize = wrbuf_len(mt->m_wr);
    return r;
}

void yaz_marc_xml(yaz_marc_t mt, int xmlmode)
{
    if (mt)
        mt->xml = xmlmode;
}

void yaz_marc_debug(yaz_marc_t mt, int level)
{
    if (mt)
        mt->debug = level;
}

void yaz_marc_iconv(yaz_marc_t mt, yaz_iconv_t cd)
{
    mt->iconv_cd = cd;
}

void yaz_marc_modify_leader(yaz_marc_t mt, size_t off, const char *str)
{
    struct yaz_marc_node *n;
    char *leader = 0;
    for (n = mt->nodes; n; n = n->next)
        if (n->which == YAZ_MARC_LEADER)
        {
            leader = n->u.leader;
            memcpy(leader+off, str, strlen(str));
            break;
        }
}

/* deprecated */
int yaz_marc_decode(const char *buf, WRBUF wr, int debug, int bsize, int xml)
{
    yaz_marc_t mt = yaz_marc_create();
    int r;

    mt->debug = debug;
    mt->xml = xml;
    r = yaz_marc_decode_wrbuf(mt, buf, bsize, wr);
    yaz_marc_destroy(mt);
    return r;
}

/* deprecated */
int marc_display_wrbuf (const char *buf, WRBUF wr, int debug, int bsize)
{
    return yaz_marc_decode(buf, wr, debug, bsize, 0);
}

/* deprecated */
int marc_display_exl (const char *buf, FILE *outf, int debug, int bsize)
{
    yaz_marc_t mt = yaz_marc_create();
    int r;

    mt->debug = debug;
    r = yaz_marc_decode_wrbuf (mt, buf, bsize, mt->m_wr);
    if (!outf)
        outf = stdout;
    if (r > 0)
        fwrite (wrbuf_buf(mt->m_wr), 1, wrbuf_len(mt->m_wr), outf);
    yaz_marc_destroy(mt);
    return r;
}

/* deprecated */
int marc_display_ex (const char *buf, FILE *outf, int debug)
{
    return marc_display_exl (buf, outf, debug, -1);
}

/* deprecated */
int marc_display (const char *buf, FILE *outf)
{
    return marc_display_ex (buf, outf, 0);
}

int yaz_marc_leader_spec(yaz_marc_t mt, const char *leader_spec)
{
    xfree(mt->leader_spec);
    mt->leader_spec = 0;
    if (leader_spec)
    {
        char dummy_leader[24];
        if (marc_exec_leader(leader_spec, dummy_leader, 24))
            return -1;
        mt->leader_spec = xstrdup(leader_spec);
    }
    return 0;
}

static int marc_exec_leader(const char *leader_spec, char *leader, size_t size)
{
    const char *cp = leader_spec;
    while (cp)
    {
        char val[21];
        int pos;
        int no_read = 0, no = 0;

        no = sscanf(cp, "%d=%20[^,]%n", &pos, val, &no_read);
        if (no < 2 || no_read < 3)
            return -1;
        if (pos < 0 || pos >= size)
            return -1;

        if (*val == '\'')
        {
            const char *vp = strchr(val+1, '\'');
            size_t len;
            
            if (!vp)
                return -1;
            len = vp-val-1;
            if (len + pos > size)
                return -1;
            memcpy(leader + pos, val+1, len);
        }
        else if (*val >= '0' && *val <= '9')
        {
            int ch = atoi(val);
            leader[pos] = ch;
        }
        else
            return -1;
        cp += no_read;
        if (*cp != ',')
            break;

        cp++;
    }
    return 0;
}


/*
 * Local variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */
