/*
* getcifsacl utility
*
* Copyright (C) Shirish Pargaonkar (shirishp@us.ibm.com) 2011
*
* Used to display a security descriptor including ACL of a file object
* that belongs to a share mounted using option cifsacl.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <string.h>
#include <getopt.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <sys/xattr.h>
#include "cifsacl.h"
#include "idmap_plugin.h"

static void *plugin_handle;
static bool plugin_loaded;
static char *execname;

static void
print_each_ace_mask(uint32_t mask)
{
	if ((mask & ALL_READ_BITS) && ((mask & EREAD) != EREAD &&
			(mask & OREAD) != OREAD && (mask & BREAD) != BREAD)) {
		printf("0x%x", mask);
		return;
	}

	if ((mask & ALL_WRITE_BITS) && (mask & EWRITE) != EWRITE) {
		printf("0x%x", mask);
		return;
	}

	if ((mask & EREAD) == EREAD || (mask & OREAD) == OREAD ||
			(mask & BREAD) == BREAD)
		printf("R");
	if ((mask & EWRITE) == EWRITE)
		printf("W");
	if ((mask & EXEC) == EXEC)
		printf("X");
	if ((mask & DELETE) == DELETE)
		printf("D");
	if ((mask & WRITE_DAC) == WRITE_DAC)
		printf("P");
	if ((mask & WRITE_OWNER) == WRITE_OWNER)
		printf("O");
}

static void
print_ace_mask(uint32_t mask, int raw)
{
	if (raw) {
		printf("0x%x\n", mask);
		return;
	}

	if (mask == FULL_CONTROL)
		printf("FULL");
	else if (mask == CHANGE)
		printf("CHANGE");
	else if (mask == DELETE)
		printf("D");
	else if (mask == EREAD)
		printf("READ");
	else if (mask & DELDHLD)
		printf("0x%x", mask);
	else
		print_each_ace_mask(mask);

	printf("\n");
	return;
}

static void
print_ace_flags(uint8_t flags, int raw)
{
	bool mflags = false;

	if (raw) {
		printf("0x%x", flags);
		return;
	}

	if (flags & OBJECT_INHERIT_FLAG) {
		mflags = true;
		printf("OI");
	}
	if (flags & CONTAINER_INHERIT_FLAG) {
		if (mflags)
			printf("|");
		else
			mflags = true;
		printf("CI");
	}
	if (flags & NO_PROPAGATE_INHERIT_FLAG) {
		if (mflags)
			printf("|");
		else
			mflags = true;
		printf("NP");
	}
	if (flags & INHERIT_ONLY_FLAG) {
		if (mflags)
			printf("|");
		else
			mflags = true;
		printf("IO");
	}
	if (flags & INHERITED_ACE_FLAG) {
		if (mflags)
			printf("|");
		else
			mflags = true;
		printf("I");
	}

	if (!mflags)
		printf("0x0");
}

static void
print_ace_type(uint8_t acetype, int raw)
{
	if (raw) {
		printf("0x%x", acetype);
		return;
	}

	switch (acetype) {
	case ACCESS_ALLOWED:
		printf("ALLOWED");
		break;
	case ACCESS_DENIED:
		printf("DENIED");
		break;
	case ACCESS_ALLOWED_OBJECT:
		printf("OBJECT_ALLOWED");
		break;
	case ACCESS_DENIED_OBJECT:
		printf("OBJECT_DENIED");
		break;
	default:
		printf("UNKNOWN");
		break;
	}
}

static void
print_sid(struct cifs_sid *csid, int raw)
{
	int i, rc;
	char *name;
	unsigned long long id_auth_val;

	if (raw || !plugin_loaded)
		goto print_sid_raw;

	rc = sid_to_str(plugin_handle, csid, &name);
	if (rc)
		goto print_sid_raw;

	printf("%s", name);
	free(name);
	return;

print_sid_raw:
	printf("S-%hhu", csid->revision);

	id_auth_val = (unsigned long long)csid->authority[5];
	id_auth_val += (unsigned long long)csid->authority[4] << 8;
	id_auth_val += (unsigned long long)csid->authority[3] << 16;
	id_auth_val += (unsigned long long)csid->authority[2] << 24;
	id_auth_val += (unsigned long long)csid->authority[1] << 32;
	id_auth_val += (unsigned long long)csid->authority[0] << 40;

	/*
	 * MS-DTYP states that if the authority is >= 2^32, then it should be
	 * expressed as a hex value.
	 */
	if (id_auth_val <= UINT_MAX)
		printf("-%llu", id_auth_val);
	else
		printf("-0x%llx", id_auth_val);

	for (i = 0; i < csid->num_subauth; i++)
		printf("-%u", le32toh(csid->sub_auth[i]));
}

static void
print_ace(struct cifs_ace *pace, char *end_of_acl, int raw)
{
	uint16_t size;

	/* make sure we can safely get to "size" */
	if (end_of_acl < (char *)pace + offsetof(struct cifs_ace, size) + 1)
		return;

	size = le16toh(pace->size);

	/* 16 == size of cifs_ace when cifs_sid has no subauths */
	if (size < 16)
		return;

	/* validate that we do not go past end of acl */
	if (end_of_acl < (char *)pace + size)
		return;

	printf("ACL:");
	print_sid((struct cifs_sid *)&pace->sid, raw);
	printf(":");
	print_ace_type(pace->type, raw);
	printf("/");
	print_ace_flags(pace->flags, raw);
	printf("/");
	print_ace_mask(le32toh(pace->access_req), raw);

	return;
}

static void
parse_dacl(struct cifs_ctrl_acl *pdacl, char *end_of_acl, int raw)
{
	int i;
	int num_aces = 0;
	int acl_size;
	char *acl_base;
	struct cifs_ace *pace;

	if (!pdacl)
		return;

	if (end_of_acl < (char *)pdacl + le16toh(pdacl->size))
		return;

	acl_base = (char *)pdacl;
	acl_size = sizeof(struct cifs_ctrl_acl);

	num_aces = le32toh(pdacl->num_aces);
	if (num_aces  > 0) {
		for (i = 0; i < num_aces; ++i) {
			pace = (struct cifs_ace *) (acl_base + acl_size);
			print_ace(pace, end_of_acl, raw);
			acl_base = (char *)pace;
			acl_size = le16toh(pace->size);
		}
	}

	return;
}

static int
parse_sid(struct cifs_sid *psid, char *end_of_acl, char *title, int raw)
{
	if (end_of_acl < (char *)psid + 8)
		return -EINVAL;

	if (title)
		printf("%s:", title);
	print_sid((struct cifs_sid *)psid, raw);
	printf("\n");

	return 0;
}

static int
parse_sec_desc(struct cifs_ntsd *pntsd, ssize_t acl_len, int raw)
{
	int rc;
	uint32_t dacloffset;
	char *end_of_acl = ((char *)pntsd) + acl_len;
	struct cifs_sid *owner_sid_ptr, *group_sid_ptr;
	struct cifs_ctrl_acl *dacl_ptr; /* no need for SACL ptr */

	if (pntsd == NULL)
		return -EIO;

	owner_sid_ptr = (struct cifs_sid *)((char *)pntsd +
				le32toh(pntsd->osidoffset));
	group_sid_ptr = (struct cifs_sid *)((char *)pntsd +
				le32toh(pntsd->gsidoffset));
	dacloffset = le32toh(pntsd->dacloffset);
	dacl_ptr = (struct cifs_ctrl_acl *)((char *)pntsd + dacloffset);
	printf("REVISION:0x%x\n", le16toh(pntsd->revision));
	printf("CONTROL:0x%x\n", le16toh(pntsd->type));

	rc = parse_sid(owner_sid_ptr, end_of_acl, "OWNER", raw);
	if (rc)
		return rc;

	rc = parse_sid(group_sid_ptr, end_of_acl, "GROUP", raw);
	if (rc)
		return rc;

	if (dacloffset)
		parse_dacl(dacl_ptr, end_of_acl, raw);
	else
		printf("No ACL\n"); /* BB grant all or default perms? */

	return 0;
}

static void
getcifsacl_usage(const char *prog)
{
	fprintf(stderr,
	"%s: Display CIFS/NTFS ACL in a security descriptor of a file object\n",
		prog);
	fprintf(stderr, "Usage: %s [option] <file_name>\n", prog);
	fprintf(stderr, "Valid options:\n");
	fprintf(stderr, "\t-h	Display this help text\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-v	Version of the program\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-r	Display raw values of the ACE fields\n");
	fprintf(stderr, "\nRefer to getcifsacl(1) manpage for details\n");
}

int
main(const int argc, char *const argv[])
{
	int c, ret = 0;
	bool raw = false;
	ssize_t attrlen;
	size_t bufsize = BUFSIZE;
	char *filename, *attrval;
	execname = basename(argv[0]);

	if (argc < 2) {
		fprintf(stderr, "%s: you must specify a filename.\n", execname);
		printf("Try `getcifsacl -h' for more information.\n");
		goto out;
	}

	while ((c = getopt_long(argc, argv, "rhv", NULL, NULL)) != -1) {
		switch (c) {
		case 'v':
			printf("Version: %s\n", VERSION);
			goto out;
		case 'r':
			raw = true;
			break;
		default:
			getcifsacl_usage(execname);
			goto out;
		}
	}

	if (optind >= argc) {
		printf("you must specify a filename after options.\n");
		printf("Usage: getcifsacl [option] <file_name>\n");
		goto out;
	} else
		filename = argv[optind];

	if (!raw && !plugin_loaded) {
		ret = init_plugin(&plugin_handle);
		if (ret)
			printf("WARNING: unable to initialize idmapping plugin: %s\n",
				plugin_errmsg);
		else
			plugin_loaded = true;
	}

cifsacl:
	if (bufsize >= XATTR_SIZE_MAX) {
		printf("buffer to allocate exceeds max size of %d\n",
				XATTR_SIZE_MAX);
		ret = -1;
		goto out;
	}

	attrval = malloc(bufsize * sizeof(char));
	if (!attrval) {
		printf("error allocating memory for attribute value buffer\n");
		ret = -1;
		goto out;
	}

	attrlen = getxattr(filename, ATTRNAME, attrval, bufsize);
	if (attrlen == -1) {
		if (errno == ERANGE) {
			free(attrval);
			bufsize += BUFSIZE;
			goto cifsacl;
		} else {
			fprintf(stderr, "getxattr failed on %s: %s\n", filename, strerror(errno) );
			free(attrval);
			ret = -1;
			goto out;
		}
	}

	parse_sec_desc((struct cifs_ntsd *)attrval, attrlen, raw);

	free(attrval);
out:
	if (plugin_loaded)
		exit_plugin(plugin_handle);
	return ret;
}
