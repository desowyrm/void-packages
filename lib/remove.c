/*-
 * Copyright (c) 2009 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <dirent.h>

#include <xbps_api.h>

static int	remove_pkg_files(prop_dictionary_t);

int
xbps_unregister_pkg(const char *pkgname)
{
	const char *rootdir;
	char *plist;
	int rv = 0;

	assert(pkgname != NULL);

	rootdir = xbps_get_rootdir();
	plist = xbps_xasprintf("%s/%s/%s", rootdir,
	     XBPS_META_PATH, XBPS_REGPKGDB);
	if (plist == NULL)
		return EINVAL;

	rv = xbps_remove_pkg_dict_from_file(pkgname, plist);
	free(plist);

	return rv;
}

static int
remove_pkg_metadir(const char *pkgname)
{
	struct dirent *dp;
	DIR *dirp;
	const char *rootdir;
	char *metadir, *path;
	int flags = 0, rv = 0;

	assert(pkgname != NULL);

	rootdir = xbps_get_rootdir();
	flags = xbps_get_flags();

	metadir = xbps_xasprintf("%s/%s/metadata/%s", rootdir,
	     XBPS_META_PATH, pkgname);
	if (metadir == NULL)
		return errno;

	dirp = opendir(metadir);
	if (dirp == NULL) {
		free(metadir);
		return errno;
	}

	while ((dp = readdir(dirp)) != NULL) {
		if ((strcmp(dp->d_name, ".") == 0) ||
		    (strcmp(dp->d_name, "..") == 0))
			continue;

		path = xbps_xasprintf("%s/%s", metadir, dp->d_name);
		if (path == NULL) {
			(void)closedir(dirp);
			free(metadir);
			return -1;
		}

		if ((rv = unlink(path)) == -1) {
			if (flags & XBPS_VERBOSE)
				printf("WARNING: can't remove %s (%s)\n",
				    pkgname, strerror(errno));
		}
		free(path);
	}
	(void)closedir(dirp);
	rv = rmdir(metadir);
	free(metadir);

	return rv;
}

static int
remove_pkg_files(prop_dictionary_t dict)
{
	prop_array_t array;
	prop_object_iterator_t iter;
	prop_object_t obj;
	prop_bool_t bobj;
	const char *file, *rootdir, *sha256, *array_str, *curftype;
	char *path = NULL;
	int i, flags = 0, rv = 0;

	rootdir = xbps_get_rootdir();
	flags = xbps_get_flags();

	/* Links */
	array = prop_dictionary_get(dict, "links");
	if (array == NULL || prop_array_count(array) == 0)
		goto files;

	iter = xbps_get_array_iter_from_dict(dict, "links");
	if (iter == NULL)
		return EINVAL;

	while ((obj = prop_object_iterator_next(iter))) {
		if (!prop_dictionary_get_cstring_nocopy(obj, "file", &file)) {
			prop_object_iterator_release(iter);
			return EINVAL;
		}
		path = xbps_xasprintf("%s/%s", rootdir, file);
		if (path == NULL) {
			prop_object_iterator_release(iter);
			return EINVAL;
		}
		if ((rv = remove(path)) == -1) {
			if (flags & XBPS_VERBOSE)
				printf("WARNING: can't remove link %s (%s)\n",
				    file, strerror(errno));
			free(path);
			continue;
		}
		if (flags & XBPS_VERBOSE)
			printf("Removed link: %s\n", file);

		free(path);
	}
	prop_object_iterator_release(iter);
	path = NULL;

files:
	/* Regular files and configuration files */
	for (i = 0; i < 2; i++) {
		if (i == 0) {
			array_str = "conf_files";
			curftype = "config file";
		} else {
			array_str = "files";
			curftype = "file";
		}
		array = prop_dictionary_get(dict, array_str);
		if (array == NULL || prop_array_count(array) == 0) {
			if (i == 0)
				continue;
			else
				goto dirs;
		}
		iter = xbps_get_array_iter_from_dict(dict, array_str);
		if (iter == NULL)
			return EINVAL;

		while ((obj = prop_object_iterator_next(iter))) {
			if (!prop_dictionary_get_cstring_nocopy(obj,
			    "file", &file)) {
				prop_object_iterator_release(iter);
				return EINVAL;
			}
			path = xbps_xasprintf("%s/%s", rootdir, file);
			if (path == NULL) {
				prop_object_iterator_release(iter);
				return EINVAL;
			}
			prop_dictionary_get_cstring_nocopy(obj,
			    "sha256", &sha256);
			rv = xbps_check_file_hash(path, sha256);
			if (rv == ENOENT) {
				free(path);
				continue;
			} else if (rv == ERANGE) {
				if (flags & XBPS_VERBOSE)
					printf("WARNING: SHA256 doesn't match "
					    "for %s %s, ignoring...\n",
					    curftype, file);
				free(path);
				continue;
			} else if (rv != 0 && rv != ERANGE) {
				free(path);
				prop_object_iterator_release(iter);
				return rv;
			}
			if ((rv = remove(path)) == -1) {
				if (flags & XBPS_VERBOSE)
					printf("WARNING: can't remove "
					    "%s %s (%s)\n", curftype, file,
					    strerror(errno));
				free(path);
				continue;
			}
			if (flags & XBPS_VERBOSE)
				printf("Removed %s: %s\n", curftype, file);

			free(path);
		}
		prop_object_iterator_release(iter);
		path = NULL;
	}

dirs:
	/* Directories */
	array = prop_dictionary_get(dict, "dirs");
	if (array == NULL || prop_array_count(array) == 0)
		return 0;

	iter = xbps_get_array_iter_from_dict(dict, "dirs");
	if (iter == NULL)
		return EINVAL;

	while ((obj = prop_object_iterator_next(iter))) {
		if ((bobj = prop_dictionary_get(obj, "keep")) != NULL) {
			/* Skip permanent directory. */
			continue;
		}
		if (!prop_dictionary_get_cstring_nocopy(obj, "file", &file)) {
			prop_object_iterator_release(iter);
			return EINVAL;
		}
		path = xbps_xasprintf("%s/%s", rootdir, file);
		if (path == NULL) {
			prop_object_iterator_release(iter);
			return EINVAL;
		}
		if ((rv = rmdir(path)) == -1) {
			if (errno == ENOTEMPTY) {
				free(path);
				continue;
			}
			if (flags & XBPS_VERBOSE) {
				printf("WARNING: can't remove "
				    "directory %s (%s)\n", file,
				    strerror(errno));
				free(path);
				continue;
			}
		}
		if (flags & XBPS_VERBOSE)
			printf("Removed directory: %s\n", file);

		free(path);
	}
	prop_object_iterator_release(iter);

	return 0;
}

int
xbps_remove_pkg(const char *pkgname, const char *version, bool update)
{
	prop_dictionary_t dict;
	const char *rootdir = xbps_get_rootdir();
	char *path, *buf;
	int rv = 0;
	bool prepostf = false;

	assert(pkgname != NULL);
	assert(version != NULL);

	/*
	 * Check if pkg is installed before anything else.
	 */
	if (xbps_check_is_installed_pkgname(pkgname) == false)
		return ENOENT;

	if (chdir(rootdir) == -1)
		return errno;

	buf = xbps_xasprintf(".%s/metadata/%s/REMOVE",
	    XBPS_META_PATH, pkgname);
	if (buf == NULL)
		return errno;

	/*
	 * Find out if the REMOVE file exists.
	 */
	if (access(buf, R_OK) == 0) {
		/*
		 * Run the pre remove action.
		 */
		prepostf = true;
		rv = xbps_file_chdir_exec(rootdir, buf, "pre", pkgname,
		    version, NULL);
		if (rv != 0) {
			printf("%s: prerm action target error (%s)\n", pkgname,
			    strerror(errno));
			free(buf);
			return rv;
		}
	}

	/*
	 * Iterate over the pkg file list dictionary and remove all
	 * files, configuration files, links and dirs.
	 */
	path = xbps_xasprintf("%s/%s/metadata/%s/%s",
	    rootdir, XBPS_META_PATH, pkgname, XBPS_PKGFILES);
	if (path == NULL) {
		free(buf);
		return errno;
	}

	dict = prop_dictionary_internalize_from_file(path);
	if (dict == NULL) {
		free(buf);
		free(path);
		return errno;
	}
	free(path);

	rv = remove_pkg_files(dict);
	if (rv != 0) {
		free(buf);
		prop_object_release(dict);
		return rv;
	}
	prop_object_release(dict);

	/*
	 * Run the post remove action if REMOVE file is there
	 * and we aren't updating a package.
	 */
	if (update == false && prepostf) {
		if ((rv = xbps_file_chdir_exec(rootdir, buf, "post",
		     pkgname, version, NULL)) != 0) {
			printf("%s: postrm action target error (%s)\n",
			    pkgname, strerror(errno));
			free(buf);
			return rv;
		}
	}
	free(buf);

	/*
	 * Update the requiredby array of all required dependencies.
	 */
	rv = xbps_requiredby_pkg_remove(pkgname);
	if (rv != 0)
		return rv;

	if (update == false) {
		/*
		 * Unregister pkg from database.
		 */
		rv = xbps_unregister_pkg(pkgname);
		if (rv != 0)
			return rv;

		/*
		 * Remove pkg metadata directory.
		 */
		return remove_pkg_metadir(pkgname);
	}

	return 0;
}
