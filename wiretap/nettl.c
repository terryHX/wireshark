/* nettl.c
 *
 * $Id$
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "wtap-int.h"
#include "file_wrappers.h"
#include "buffer.h"
#include "nettl.h"

static guchar nettl_magic_hpux9[12] = {
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xD0, 0x00
};
static guchar nettl_magic_hpux10[12] = {
    0x54, 0x52, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80
};

/* HP nettl record header for the SX25L2 subsystem - The FCS is not included in the file. */
struct nettlrec_sx25l2_hdr {
    guint8	xxa[8];
    guint8	from_dce;
    guint8	xxb[55];
    guint8	caplen[2];
    guint8	length[2];
    guint8	xxc[4];
    guint8	sec[4];
    guint8	usec[4];
    guint8	xxd[4];
};

/* HP nettl record header for the NS_LS_IP subsystem */
/* This also works for BASE100 and GSC100BT */
struct nettlrec_ns_ls_ip_hdr {
    guint8	xxa[8];
    guint8	rectype;
    guint8	xxb[19];
    guint8	caplen[4];
    guint8	length[4];
    guint8	sec[4];
    guint8	usec[4];
    guint8	xxc[16];
};


/* header is followed by data and once again the total length (2 bytes) ! */


/* NL_LS_DRIVER :
The following shows what the header looks like for NS_LS_DRIVER
The capture was taken on HPUX11 and for a 100baseT interface.

000080 00 44 00 0b 00 00 00 02 00 00 00 00 20 00 00 00
000090 00 00 00 00 00 00 04 06 00 00 00 00 00 00 00 00
0000a0 00 00 00 74 00 00 00 74 3c e3 76 19 00 06 34 63
0000b0 ff ff ff ff 00 00 00 00 00 00 00 00 ff ff ff ff
0000c0 00 00 00 00 00 00 01 02 00 5c 00 5c ff ff ff ff
0000d0 3c e3 76 19 00 06 34 5a 00 0b 00 14 <here starts the MAC heder>

Each entry starts with 0x0044000b

The values 0x005c at position 0x0000c8 and 0x0000ca matches the number of bytes in
the packet up to the next entry, which starts with 0x00440b again. These probably
indicate the real and captured length of the packet (order unknown)

The values 0x00000074 at positions 0x0000a0 and 0x0000a4 seems to indicate
the same number as positions 0x0000c8 and 0x0000ca but added with 24.
Perhaps we have here two layers of headers.
The first layer is fixed and consists of all the bytes from 0x000084 up to and
including 0x0000c3 which is a generic header for all packets captured from any
device. This header might be of fixed size 64 bytes and there might be something in
it which indicates the type of the next header which is link type specific.
Following this header there is another header for the 100baseT interface which
in this case is 24 bytes long spanning positions 0x0000c4 to 0x0000db.

When someone reports that the loading of the captures breaks, we can compare
this header above with what he/she got to learn how to distinguish between different
types of link specific headers.


For now:
The first header seems to be
	a normal nettlrec_ns_ls_ip_hdr

The header for 100baseT seems to be
	0-3	unknown
	4-5	captured length
	6-7	actual length
	8-11	unknown
	12-15	secs
	16-19	usecs
	20-23	unknown
*/
struct nettlrec_ns_ls_drv_eth_hdr {
    guint8	xxa[4];
    guint8      caplen[2];
    guint8      length[2];
    guint8	xxb[4];
    guint8	sec[4];
    guint8	usec[4];
    guint8	xxc[4];
};


static gboolean nettl_read(wtap *wth, int *err, gchar **err_info,
		long *data_offset);
static gboolean nettl_seek_read(wtap *wth, long seek_off,
		union wtap_pseudo_header *pseudo_header, guchar *pd,
		int length, int *err, gchar **err_info);
static int nettl_read_rec_header(wtap *wth, FILE_T fh,
		struct wtap_pkthdr *phdr, union wtap_pseudo_header *pseudo_header,
		int *err, gchar **err_info, gboolean *fddihack);
static gboolean nettl_read_rec_data(FILE_T fh, guchar *pd, int length,
		int *err, gboolean fddihack);
static void nettl_close(wtap *wth);

int nettl_open(wtap *wth, int *err, gchar **err_info _U_)
{
    char magic[12], os_vers[2];
    int bytes_read;

    /* Read in the string that should be at the start of a HP file */
    errno = WTAP_ERR_CANT_READ;
    bytes_read = file_read(magic, 1, 12, wth->fh);
    if (bytes_read != 12) {
    	*err = file_error(wth->fh);
	if (*err != 0)
	    return -1;
	return 0;
    }

    if (memcmp(magic, nettl_magic_hpux9, 12) &&
        memcmp(magic, nettl_magic_hpux10, 12)) {
	return 0;
    }

    if (file_seek(wth->fh, 0x63, SEEK_SET, err) == -1)
	return -1;
    wth->data_offset = 0x63;
    bytes_read = file_read(os_vers, 1, 2, wth->fh);
    if (bytes_read != 2) {
	*err = file_error(wth->fh);
	if (*err != 0)
	    return -1;
	return 0;
    }

    if (file_seek(wth->fh, 0x80, SEEK_SET, err) == -1)
	return -1;
    wth->data_offset = 0x80;

    /* This is an nettl file */
    wth->file_type = WTAP_FILE_NETTL;
    wth->capture.nettl = g_malloc(sizeof(nettl_t));
    if (os_vers[0] == '1' && os_vers[1] == '1')
	wth->capture.nettl->is_hpux_11 = TRUE;
    else
	wth->capture.nettl->is_hpux_11 = FALSE;
    wth->subtype_read = nettl_read;
    wth->subtype_seek_read = nettl_seek_read;
    wth->subtype_close = nettl_close;
    wth->snapshot_length = 0;	/* not available in header, only in frame */

    return 1;
}

/* Read the next packet */
static gboolean nettl_read(wtap *wth, int *err, gchar **err_info,
    long *data_offset)
{
    int ret;
    gboolean fddihack=FALSE;

    /* Read record header. */
    *data_offset = wth->data_offset;
    ret = nettl_read_rec_header(wth, wth->fh, &wth->phdr, &wth->pseudo_header,
        err, err_info, &fddihack);
    if (ret <= 0) {
	/* Read error or EOF */
	return FALSE;
    }
    wth->data_offset += ret;

    /*
     * If the per-file encapsulation isn't known, set it to this
     * packet's encapsulation.
     *
     * If it *is* known, and it isn't this packet's encapsulation,
     * set it to WTAP_ENCAP_PER_PACKET, as this file doesn't
     * have a single encapsulation for all packets in the file.
     */
    if (wth->file_encap == WTAP_ENCAP_UNKNOWN)
	wth->file_encap = wth->phdr.pkt_encap;
    else {
	if (wth->file_encap != wth->phdr.pkt_encap)
	    wth->file_encap = WTAP_ENCAP_PER_PACKET;
    }

    /*
     * Read the packet data.
     */
    buffer_assure_space(wth->frame_buffer, wth->phdr.caplen);
    if (!nettl_read_rec_data(wth->fh, buffer_start_ptr(wth->frame_buffer),
		wth->phdr.caplen, err, fddihack))
	return FALSE;	/* Read error */
    wth->data_offset += wth->phdr.caplen;
    return TRUE;
}

static gboolean
nettl_seek_read(wtap *wth, long seek_off,
		union wtap_pseudo_header *pseudo_header, guchar *pd,
		int length, int *err, gchar **err_info)
{
    int ret;
    struct wtap_pkthdr phdr;
    gboolean fddihack=FALSE;

    if (file_seek(wth->random_fh, seek_off, SEEK_SET, err) == -1)
	return FALSE;

    /* Read record header. */
    ret = nettl_read_rec_header(wth, wth->random_fh, &phdr, pseudo_header,
        err, err_info, &fddihack);
    if (ret <= 0) {
	/* Read error or EOF */
	if (ret == 0) {
	    /* EOF means "short read" in random-access mode */
	    *err = WTAP_ERR_SHORT_READ;
	}
	return FALSE;
    }

    /*
     * Read the packet data.
     */
    return nettl_read_rec_data(wth->random_fh, pd, length, err, fddihack);
}

static int
nettl_read_rec_header(wtap *wth, FILE_T fh, struct wtap_pkthdr *phdr,
		union wtap_pseudo_header *pseudo_header, int *err,
		gchar **err_info, gboolean *fddihack)
{
    int bytes_read;
    struct nettlrec_sx25l2_hdr lapb_hdr;
    struct nettlrec_ns_ls_ip_hdr ip_hdr;
    struct nettlrec_ns_ls_drv_eth_hdr drv_eth_hdr;
    guint16 length;
    int offset = 0;
    int encap;
    int padlen;
    guint8 dummy[4];
    guchar dummyc[12];

    errno = WTAP_ERR_CANT_READ;
    bytes_read = file_read(dummy, 1, 4, fh);
    if (bytes_read != 4) {
	*err = file_error(fh);
	if (*err != 0)
	    return -1;
	if (bytes_read != 0) {
	    *err = WTAP_ERR_SHORT_READ;
	    return -1;
	}
	return 0;
    }
    offset += 4;
    encap=dummy[3];

    switch (encap) {
	case NETTL_SUBSYS_LAN100 :
	case NETTL_SUBSYS_EISA100BT :
	case NETTL_SUBSYS_BASE100 :
	case NETTL_SUBSYS_GSC100BT :
	case NETTL_SUBSYS_PCI100BT :
	case NETTL_SUBSYS_SPP100BT :
	case NETTL_SUBSYS_GELAN :
	case NETTL_SUBSYS_BTLAN :
	case NETTL_SUBSYS_INTL100 :
	case NETTL_SUBSYS_IGELAN :
	case NETTL_SUBSYS_IETHER :
	case NETTL_SUBSYS_HPPB_FDDI :
	case NETTL_SUBSYS_EISA_FDDI :
        case NETTL_SUBSYS_PCI_FDDI :
        case NETTL_SUBSYS_HSC_FDDI :
        case NETTL_SUBSYS_TOKEN :
        case NETTL_SUBSYS_PCI_TR :
	case NETTL_SUBSYS_NS_LS_IP :
	case NETTL_SUBSYS_NS_LS_LOOPBACK :
	case NETTL_SUBSYS_NS_LS_TCP :
	case NETTL_SUBSYS_NS_LS_UDP :
	case NETTL_SUBSYS_HP_APAPORT :
	case NETTL_SUBSYS_HP_APALACP :
	case NETTL_SUBSYS_NS_LS_IPV6 :
	case NETTL_SUBSYS_NS_LS_ICMPV6 :
	case NETTL_SUBSYS_NS_LS_ICMP :
	    if( (encap == NETTL_SUBSYS_NS_LS_IP)
	     || (encap == NETTL_SUBSYS_NS_LS_LOOPBACK)
	     || (encap == NETTL_SUBSYS_NS_LS_UDP)
	     || (encap == NETTL_SUBSYS_NS_LS_TCP)
	     || (encap == NETTL_SUBSYS_NS_LS_IPV6)) {
		phdr->pkt_encap = WTAP_ENCAP_RAW_IP;
	    } else if (encap == NETTL_SUBSYS_NS_LS_ICMP) {
		phdr->pkt_encap = WTAP_ENCAP_RAW_ICMP;
	    } else if (encap == NETTL_SUBSYS_NS_LS_ICMPV6) {
		phdr->pkt_encap = WTAP_ENCAP_RAW_ICMPV6;
	    } else if( (encap == NETTL_SUBSYS_HPPB_FDDI)
		    || (encap == NETTL_SUBSYS_EISA_FDDI)
		    || (encap == NETTL_SUBSYS_PCI_FDDI)
		    || (encap == NETTL_SUBSYS_HSC_FDDI) ) {
		phdr->pkt_encap = WTAP_ENCAP_FDDI;
	    } else if( (encap == NETTL_SUBSYS_PCI_TR)
		    || (encap == NETTL_SUBSYS_TOKEN) ) {
		phdr->pkt_encap = WTAP_ENCAP_TOKEN_RING;
	    } else {
		phdr->pkt_encap = WTAP_ENCAP_ETHERNET;
		/* We assume there's no FCS in this frame. */
		pseudo_header->eth.fcs_len = 0;
	    }

	    bytes_read = file_read(&ip_hdr, 1, sizeof ip_hdr, fh);
	    if (bytes_read != sizeof ip_hdr) {
		*err = file_error(fh);
		if (*err != 0)
		    return -1;
		if (bytes_read != 0) {
		    *err = WTAP_ERR_SHORT_READ;
		    return -1;
		}
		return 0;
	    }
	    offset += sizeof ip_hdr;

	    /* The packet header in HP-UX 11 nettl traces is 4 octets longer than
	     * HP-UX 9 and 10 */
	    if (wth->capture.nettl->is_hpux_11) {
		bytes_read = file_read(dummy, 1, 4, fh);
		if (bytes_read != 4) {
		    *err = file_error(fh);
		    if (*err != 0)
			return -1;
		    if (bytes_read != 0) {
			*err = WTAP_ERR_SHORT_READ;
			return -1;
		    }
		    return 0;
		}
		offset += 4;
	    }

	    /* HPPB FDDI has different inbound vs outbound trace records */
	    if (encap == NETTL_SUBSYS_HPPB_FDDI) {
                if (ip_hdr.rectype == NETTL_HDR_PDUIN) {
                   /* inbound is very strange...
                      there are an extra 3 bytes after the DSAP and SSAP
                      for SNAP frames ???
                   */
                   *fddihack=TRUE;
		   length = pntohl(&ip_hdr.length);
		   if (length <= 0)
		       return 0;
		   phdr->len = length;
		   length = pntohl(&ip_hdr.caplen);
		   phdr->caplen = length;
                } else {
	           /* outbound appears to have variable padding */
		   bytes_read = file_read(dummyc, 1, 9, fh);
		   if (bytes_read != 9) {
		       *err = file_error(fh);
		       if (*err != 0)
			   return -1;
		       if (bytes_read != 0) {
			   *err = WTAP_ERR_SHORT_READ;
			   return -1;
		       }
		       return 0;
		   }
                   /* padding is usually either a total 11 or 16 bytes??? */
		   padlen = (int)dummyc[8];
		   bytes_read = file_read(dummy, 1, padlen, fh);
		   if (bytes_read != padlen) {
		       *err = file_error(fh);
		       if (*err != 0)
			   return -1;
		       if (bytes_read != 0) {
			   *err = WTAP_ERR_SHORT_READ;
			   return -1;
		       }
		       return 0;
		   }
		   padlen += 9;
		   offset += padlen;
		   length = pntohl(&ip_hdr.length);
		   if (length <= 0)
			   return 0;
		   phdr->len = length - padlen;
		   length = pntohl(&ip_hdr.caplen);
		   phdr->caplen = length - padlen;
               }
	    } else if ( (encap == NETTL_SUBSYS_PCI_FDDI)
	             || (encap == NETTL_SUBSYS_EISA_FDDI)
	             || (encap == NETTL_SUBSYS_HSC_FDDI) ) {
	        /* other flavor FDDI cards have an extra 3 bytes of padding */
		bytes_read = file_read(dummy, 1, 3, fh);
		if (bytes_read != 3) {
		    *err = file_error(fh);
		    if (*err != 0)
			return -1;
		    if (bytes_read != 0) {
			*err = WTAP_ERR_SHORT_READ;
			return -1;
		    }
		    return 0;
		}
		offset += 3;
		length = pntohl(&ip_hdr.length);
		if (length <= 0)
			return 0;
		phdr->len = length - 3;
		length = pntohl(&ip_hdr.caplen);
		phdr->caplen = length - 3;
	    } else if (encap == NETTL_SUBSYS_NS_LS_LOOPBACK) {
	        /* LOOPBACK has an extra 26 bytes of padding */
		bytes_read = file_read(dummy, 1, 26, fh);
		if (bytes_read != 26) {
		    *err = file_error(fh);
		    if (*err != 0)
			return -1;
		    if (bytes_read != 0) {
			*err = WTAP_ERR_SHORT_READ;
			return -1;
		    }
		    return 0;
		}
		offset += 26;
		length = pntohl(&ip_hdr.length);
		if (length <= 0)
			return 0;
		phdr->len = length - 26;
		length = pntohl(&ip_hdr.caplen);
		phdr->caplen = length - 26;
	    } else {
		length = pntohl(&ip_hdr.length);
		if (length <= 0)
		    return 0;
		phdr->len = length;
		length = pntohl(&ip_hdr.caplen);
		phdr->caplen = length;
	    }

	    phdr->ts.tv_sec = pntohl(&ip_hdr.sec);
	    phdr->ts.tv_usec = pntohl(&ip_hdr.usec);
	    break;
	case NETTL_SUBSYS_NS_LS_DRIVER :
	    bytes_read = file_read(&ip_hdr, 1, sizeof ip_hdr, fh);
	    if (bytes_read != sizeof ip_hdr) {
		*err = file_error(fh);
		if (*err != 0)
		    return -1;
		if (bytes_read != 0) {
		    *err = WTAP_ERR_SHORT_READ;
		    return -1;
		}
		return 0;
	    }
	    offset += sizeof ip_hdr;

	    /* The packet header in HP-UX 11 nettl traces is 4 octets longer than
	     * HP-UX 9 and 10 */
	    if (wth->capture.nettl->is_hpux_11) {
		bytes_read = file_read(dummy, 1, 4, fh);
		if (bytes_read != 4) {
		    *err = file_error(fh);
		    if (*err != 0)
			return -1;
		    if (bytes_read != 0) {
			*err = WTAP_ERR_SHORT_READ;
			return -1;
		    }
		    return 0;
		}
		offset += 4;
	    }

	    /* XXX we dont know how to identify this as ethernet frames, so
	       we assumes everything is. We will crash and burn for anything else */
	    /* for encapsulated 100baseT we do this */
	    phdr->pkt_encap = WTAP_ENCAP_ETHERNET;
	    /* We assume there's no FCS in this frame. */
	    pseudo_header->eth.fcs_len = 0;
	    bytes_read = file_read(&drv_eth_hdr, 1, sizeof drv_eth_hdr, fh);
	    if (bytes_read != sizeof drv_eth_hdr) {
		*err = file_error(fh);
		if (*err != 0)
		    return -1;
		if (bytes_read != 0) {
		    *err = WTAP_ERR_SHORT_READ;
		    return -1;
		}
		return 0;
	    }
	    offset += sizeof drv_eth_hdr;

	    length = pntohs(&drv_eth_hdr.length); 
	    if (length <= 0) return 0;
	    phdr->len = length;
	    length = pntohs(&drv_eth_hdr.caplen);
	    phdr->caplen = length;

	    phdr->ts.tv_sec = pntohl(&ip_hdr.sec);
	    phdr->ts.tv_usec = pntohl(&ip_hdr.usec);
	    break;
	case NETTL_SUBSYS_SX25L2 :
	    phdr->pkt_encap = WTAP_ENCAP_LAPB;
	    bytes_read = file_read(&lapb_hdr, 1, sizeof lapb_hdr, fh);
	    if (bytes_read != sizeof lapb_hdr) {
		*err = file_error(fh);
		if (*err != 0)
		    return -1;
		if (bytes_read != 0) {
		    *err = WTAP_ERR_SHORT_READ;
		    return -1;
		}
		return 0;
	    }
	    offset += sizeof lapb_hdr;

	    if (wth->capture.nettl->is_hpux_11) {
		bytes_read = file_read(dummy, 1, 4, fh);
		if (bytes_read != 4) {
		    *err = file_error(fh);
		    if (*err != 0)
			return -1;
		    if (bytes_read != 0) {
			*err = WTAP_ERR_SHORT_READ;
			return -1;
		    }
		    return 0;
		}
		offset += 4;
	    }

	    length = pntohs(&lapb_hdr.length);
	    if (length <= 0) return 0;
	    phdr->len = length;
	    phdr->caplen = length;

	    phdr->ts.tv_sec = pntohl(&lapb_hdr.sec);
	    phdr->ts.tv_usec = pntohl(&lapb_hdr.usec);
	    pseudo_header->x25.flags =
		(lapb_hdr.from_dce & 0x20 ? FROM_DCE : 0x00);
	    break;
	default:
	    *err = WTAP_ERR_UNSUPPORTED_ENCAP;
	    *err_info = g_strdup_printf("nettl: subsystem %u unknown or unsupported",
		    encap);
	    return -1;
    }
    return offset;
}

static gboolean
nettl_read_rec_data(FILE_T fh, guchar *pd, int length, int *err, gboolean fddihack)
{
    int bytes_read;
    guchar *p=NULL;
    guint8 dummy[3];

    if (fddihack == TRUE) {
       /* read in FC, dest, src, DSAP and SSAP */
       if (file_read(pd, 1, 15, fh) == 15) {
          if (pd[13] == 0xAA) {
             /* it's SNAP, have to eat 3 bytes??? */
             if (file_read(dummy, 1, 3, fh) == 3) {
                p=pd+15;
                bytes_read = file_read(p, 1, length-18, fh);
                bytes_read += 18;
             } else {
                bytes_read = -1;
             }
          } else {
             /* not SNAP */
             p=pd+15;
             bytes_read = file_read(p, 1, length-15, fh);
             bytes_read += 15;
          }
       } else
          bytes_read = -1;
    } else
       bytes_read = file_read(pd, 1, length, fh);

    if (bytes_read != length) {
	*err = file_error(fh);
	if (*err == 0)
	    *err = WTAP_ERR_SHORT_READ;
	return FALSE;
    }
    return TRUE;
}

static void nettl_close(wtap *wth)
{
    g_free(wth->capture.nettl);
}
