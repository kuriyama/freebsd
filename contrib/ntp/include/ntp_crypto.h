/*
 * ntp_crypto.h - definitions for cryptographic operations
 */
#ifdef OPENSSL
#include "openssl/evp.h"
/*
 * The following bits are set by the CRYPTO_ASSOC message from
 * the server and are not modified by the client.
 */
#define CRYPTO_FLAG_ENAB  0x0001 /* crypto enable */
#define CRYPTO_FLAG_TAI   0x0002 /* leapseconds table */

#define CRYPTO_FLAG_PRIV  0x0010 /* PC identity scheme */
#define CRYPTO_FLAG_IFF   0x0020 /* IFF identity scheme */
#define CRYPTO_FLAG_GQ	  0x0040 /* GQ identity scheme */
#define	CRYPTO_FLAG_MV	  0x0080 /* MV identity scheme */
#define CRYPTO_FLAG_MASK  0x00f0 /* identity scheme mask */
	
/*
 * The following bits are used by the client during the protocol
 * exchange.
 */
#define CRYPTO_FLAG_VALID 0x0100 /* public key verified */
#define CRYPTO_FLAG_VRFY  0x0200 /* identity verified */
#define CRYPTO_FLAG_PROV  0x0400 /* signature verified */
#define CRYPTO_FLAG_AGREE 0x0800 /* cookie verifed */
#define CRYPTO_FLAG_AUTO  0x1000 /* autokey verified */
#define CRYPTO_FLAG_SIGN  0x2000 /* certificate signed */
#define CRYPTO_FLAG_LEAP  0x4000 /* leapseconds table verified */

/*
 * Flags used for certificate management
 */
#define	CERT_TRUST	0x01	/* certificate is trusted */
#define CERT_SIGN	0x02	/* certificate is signed */
#define CERT_VALID	0x04	/* certificate is valid */
#define CERT_PRIV	0x08	/* certificate is private */
#define CERT_ERROR	0x80	/* certificate has errors */

/*
 * Extension field definitions
 */
#define	CRYPTO_MAXLEN	1024	/* max extension field length */
#define CRYPTO_VN	2	/* current protocol version number */
#define CRYPTO_CMD(x)	(((CRYPTO_VN << 8) | (x)) << 16)
#define CRYPTO_NULL	CRYPTO_CMD(0) /* no operation */
#define CRYPTO_ASSOC	CRYPTO_CMD(1) /* association */
#define CRYPTO_CERT	CRYPTO_CMD(2) /* certificate */
#define CRYPTO_COOK	CRYPTO_CMD(3) /* cookie value */
#define CRYPTO_AUTO	CRYPTO_CMD(4) /* autokey values */
#define CRYPTO_TAI	CRYPTO_CMD(5) /* leapseconds table */
#define	CRYPTO_SIGN	CRYPTO_CMD(6) /* certificate sign */
#define CRYPTO_IFF	CRYPTO_CMD(7) /* IFF identity scheme */
#define CRYPTO_GQ	CRYPTO_CMD(8) /* GQ identity scheme */
#define	CRYPTO_MV	CRYPTO_CMD(9) /* MV identity scheme */
#define CRYPTO_RESP	0x80000000 /* response */
#define CRYPTO_ERROR	0x40000000 /* error */

/*
 * Autokey event codes
 */
#define XEVNT_CMD(x)	(CRPT_EVENT | (x))
#define XEVNT_OK	XEVNT_CMD(0) /* success */
#define XEVNT_LEN	XEVNT_CMD(1) /* bad field format or length */
#define XEVNT_TSP	XEVNT_CMD(2) /* bad timestamp */
#define XEVNT_FSP	XEVNT_CMD(3) /* bad filestamp */
#define XEVNT_PUB	XEVNT_CMD(4) /* bad or missing public key */
#define XEVNT_MD	XEVNT_CMD(5) /* unsupported digest type */
#define XEVNT_KEY	XEVNT_CMD(6) /* unsupported identity type */
#define XEVNT_SGL	XEVNT_CMD(7) /* bad signature length */
#define XEVNT_SIG	XEVNT_CMD(8) /* signature not verified */
#define XEVNT_VFY	XEVNT_CMD(9) /* certificate not verified */
#define XEVNT_PER	XEVNT_CMD(10) /* host certificate expired */
#define XEVNT_CKY	XEVNT_CMD(11) /* bad or missing cookie */
#define XEVNT_DAT	XEVNT_CMD(12) /* bad or missing leapseconds table */
#define XEVNT_CRT	XEVNT_CMD(13) /* bad or missing certificate */
#define XEVNT_ID	XEVNT_CMD(14) /* bad or missing group key */
#define	XEVNT_ERR	XEVNT_CMD(15) /* protocol error */
#define	XEVNT_SRV	XEVNT_CMD(16) /* server certificate expired */

/*
 * Configuration codes
 */
#define CRYPTO_CONF_NONE  0	/* nothing doing */
#define CRYPTO_CONF_PRIV  1	/* host keys file name */
#define CRYPTO_CONF_SIGN  2	/* signature keys file name */
#define CRYPTO_CONF_LEAP  3	/* leapseconds table file name */
#define CRYPTO_CONF_KEYS  4	/* keys directory path */
#define CRYPTO_CONF_CERT  5	/* certificate file name */
#define CRYPTO_CONF_RAND  6	/* random seed file name */
#define	CRYPTO_CONF_TRST  7	/* specify trust */
#define CRYPTO_CONF_IFFPAR 8	/* IFF parameters file name */
#define CRYPTO_CONF_GQPAR 9	/* GQ parameters file name */
#define	CRYPTO_CONF_MVPAR 10	/* GQ parameters file name */
#define CRYPTO_CONF_PW	  11	/* private key password */
#define	CRYPTO_CONF_IDENT 12	/* specify identity scheme */

/*
 * Miscellaneous crypto stuff
 */
#define NTP_MAXSESSION	100	/* maximum session key list entries */
#define NTP_AUTOMAX	13	/* log2 default max session key life */
#define KEY_REVOKE	16	/* log2 default key revoke timeout */
#define NTP_MAXEXTEN	1024	/* maximum extension field size */
#define	TAI_1972	10	/* initial TAI offset (s) */

/*
 * The autokey structure holds the values used to authenticate key IDs.
 */
struct autokey {		/* network byte order */
	keyid_t	key;		/* key ID */
	int32	seq;		/* key number */
};

/*
 * The value structure holds variable length data such as public
 * key, agreement parameters, public valule and leapsecond table.
 * They are in network byte order.
 */
struct value {			/* network byte order */
	tstamp_t tstamp;	/* timestamp */
	tstamp_t fstamp;	/* filestamp */
	u_int32	vallen;		/* value length */
	u_char	*ptr;		/* data pointer (various) */
	u_int32	siglen;		/* signature length */
	u_char	*sig;		/* signature */
};

/*
 * The packet extension field structures are used to hold values
 * and signatures in network byte order.
 */
struct exten {
	u_int32	opcode;		/* opcode */
	u_int32	associd;	/* association ID */
	u_int32	tstamp;		/* timestamp */
	u_int32	fstamp;		/* filestamp */
	u_int32	vallen;		/* value length */
	u_int32	pkt[1];		/* start of value field */
};

/*
 * The certificate info/value structure
 */
struct cert_info {
	struct cert_info *link;	/* forward link */
	u_int	flags;		/* flags that wave */
	EVP_PKEY *pkey;		/* generic key */
	long	version;	/* X509 version */
	int	nid;		/* signature/digest ID */
	const EVP_MD *digest;	/* message digest algorithm */
	u_long	serial;		/* serial number */
	tstamp_t first;		/* not valid before */
	tstamp_t last;		/* not valid after */
	char	*subject;	/* subject common name */
	char	*issuer;	/* issuer common name */
	u_char	*grpkey;	/* GQ group key */
	u_int	grplen;		/* GQ group key length */
	struct value cert;	/* certificate/value */
};

/*
 * Cryptographic values
 */
extern	char	*keysdir;	/* crypto keys directory */
extern	u_int	crypto_flags;	/* status word */
extern	struct value hostval;	/* host name/value */
extern	struct cert_info *cinfo; /* host certificate information */
extern	struct value tai_leap;	/* leapseconds table */
#endif /* OPENSSL */
