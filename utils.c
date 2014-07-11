/* MiniDLNA media server
 * Copyright (C) 2008-2009  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "minidlnatypes.h"
#include "upnpglobalvars.h"
#include "log.h"

inline int
strcatf(struct string_s *str, const char *fmt, ...)
{
	int ret;
	int size;
	va_list ap;

	if (str->off >= str->size)
		return 0;

	va_start(ap, fmt);
	size = str->size - str->off;
	ret = vsnprintf(str->data + str->off, size, fmt, ap);
	str->off += MIN(ret, size);
	va_end(ap);

	return ret;
}

inline void
strncpyt(char *dst, const char *src, size_t len)
{
	strncpy(dst, src, len);
	dst[len-1] = '\0';
}

inline int
xasprintf(char **strp, char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vasprintf(strp, fmt, args);
	va_end(args);
	if( ret < 0 )
	{
		DPRINTF(E_WARN, L_GENERAL, "xasprintf: allocation failed\n");
		*strp = NULL;
	}
	return ret;
}

int
ends_with(const char * haystack, const char * needle)
{
	const char * end;
	int nlen = strlen(needle);
	int hlen = strlen(haystack);

	if( nlen > hlen )
		return 0;
 	end = haystack + hlen - nlen;

	return (strcasecmp(end, needle) ? 0 : 1);
}

char *
trim(char *str)
{
	int i;
	int len;

	if (!str)
		return(NULL);

	len = strlen(str);
	for (i=len-1; i >= 0 && isspace(str[i]); i--)
	{
		str[i] = '\0';
		len--;
	}
	while (isspace(*str))
	{
		str++;
		len--;
	}

	if (str[0] == '"' && str[len-1] == '"')
	{
		str[0] = '\0';
		str[len-1] = '\0';
		str++;
	}

	return str;
}

/* Find the first occurrence of p in s, where s is terminated by t */
char *
strstrc(const char *s, const char *p, const char t)
{
	char *endptr;
	size_t slen, plen;

	endptr = strchr(s, t);
	if (!endptr)
		return strstr(s, p);

	plen = strlen(p);
	slen = endptr - s;
	while (slen >= plen)
	{
		if (*s == *p && strncmp(s+1, p+1, plen-1) == 0)
			return (char*)s;
		s++;
		slen--;
	}

	return NULL;
} 

char *
strcasestrc(const char *s, const char *p, const char t)
{
	char *endptr;
	size_t slen, plen;

	endptr = strchr(s, t);
	if (!endptr)
		return strcasestr(s, p);

	plen = strlen(p);
	slen = endptr - s;
	while (slen >= plen)
	{
		if (*s == *p && strncasecmp(s+1, p+1, plen-1) == 0)
			return (char*)s;
		s++;
		slen--;
	}

	return NULL;
} 

char *
modifyString(char * string, const char * before, const char * after)
{
	int oldlen, newlen, chgcnt = 0;
	char *s, *p;

	oldlen = strlen(before);
	newlen = strlen(after);
	if( newlen > oldlen )
	{
		s = string;
		while( (p = strstr(s, before)) )
		{
			chgcnt++;
			s = p+oldlen;
		}
		s = realloc(string, strlen(string)+((newlen-oldlen)*chgcnt)+1);
		/* If we failed to realloc, return the original alloc'd string */
		if( s )
			string = s;
		else
			return string;
	}

	s = string;
	while( s )
	{
		p = strcasestr(s, before);
		if( !p )
			return string;
		memmove(p + newlen, p + oldlen, strlen(p + oldlen) + 1);
		memcpy(p, after, newlen);
		s = p + newlen;
	}

	return string;
}

char *
unescape_tag(const char *tag, int force_alloc)
{
	char *esc_tag = NULL;

	if( strstr(tag, "&amp;") || strstr(tag, "&lt;") || strstr(tag, "&gt;")
			|| strstr(tag, "&quot;") )
	{
		esc_tag = strdup(tag);
		esc_tag = modifyString(esc_tag, "&amp;", "&");
		esc_tag = modifyString(esc_tag, "&lt;", "<");
		esc_tag = modifyString(esc_tag, "&gt;", ">");
		esc_tag = modifyString(esc_tag, "&quot;", "\"");
	}
	else if( force_alloc )
		esc_tag = strdup(tag);

	return esc_tag;
}

char *
escape_tag(const char *tag, int force_alloc)
{
	char *esc_tag = NULL;

	if( strchr(tag, '&') || strchr(tag, '<') || strchr(tag, '>') || strchr(tag, '"') )
	{
		esc_tag = strdup(tag);
		esc_tag = modifyString(esc_tag, "&", "&amp;amp;");
		esc_tag = modifyString(esc_tag, "<", "&amp;lt;");
		esc_tag = modifyString(esc_tag, ">", "&amp;gt;");
		esc_tag = modifyString(esc_tag, "\"", "&amp;quot;");
	}
	else if( force_alloc )
		esc_tag = strdup(tag);

	return esc_tag;
}

void
strip_ext(char * name)
{
	char * period;

	period = strrchr(name, '.');
#ifdef XIAODU_NAS
	if( period && period != name)
#else
	if( period )
#endif
		*period = '\0';
}

/* Code basically stolen from busybox */
int
make_dir(char * path, mode_t mode)
{
	char * s = path;
	char c;
	struct stat st;

	do {
		c = '\0';

		/* Before we do anything, skip leading /'s, so we don't bother
		 * trying to create /. */
		while (*s == '/')
			++s;

		/* Bypass leading non-'/'s and then subsequent '/'s. */
		while (*s) {
			if (*s == '/') {
				do {
					++s;
				} while (*s == '/');
				c = *s;     /* Save the current char */
				*s = '\0';     /* and replace it with nul. */
				break;
			}
			++s;
		}

		if (mkdir(path, mode) < 0) {
			/* If we failed for any other reason than the directory
			 * already exists, output a diagnostic and return -1.*/
			if ((errno != EEXIST && errno != EISDIR)
			    || (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))) {
				DPRINTF(E_WARN, L_GENERAL, "make_dir: cannot create directory '%s'\n", path);
				if (c)
					*s = c;
				return -1;
			}
		}
	        if (!c)
			return 0;

		/* Remove any inserted nul from the path. */
		*s = c;

	} while (1);
}

/* Simple, efficient hash function from Daniel J. Bernstein */
unsigned int
DJBHash(const char *str, int len)
{
	unsigned int hash = 5381;
	unsigned int i = 0;

	for(i = 0; i < len; str++, i++)
	{
		hash = ((hash << 5) + hash) + (*str);
	}

	return hash;
}
#ifdef BAIDU_DMS_OPT
char *
title_to_ext(const char * title)
{
	char * period;
	period = strrchr(title, '.');
	if(period)
		period++;
	return period;
}
#endif
const char *
mime_to_ext(const char * mime)
{
	switch( *mime )
	{
		/* Audio extensions */
		case 'a':
			if( strcmp(mime+6, "mpeg") == 0 )
				return "mp3";
			else if( strcmp(mime+6, "mp4") == 0 )
				return "m4a";
			else if( strcmp(mime+6, "x-ms-wma") == 0 )
				return "wma";
			else if( strcmp(mime+6, "x-flac") == 0 )
				return "flac";
			else if( strcmp(mime+6, "flac") == 0 )
				return "flac";
			else if( strcmp(mime+6, "x-wav") == 0 )
				return "wav";
			else if( strncmp(mime+6, "L16", 3) == 0 )
				return "pcm";
			else if( strcmp(mime+6, "3gpp") == 0 )
				return "3gp";
			else if( strcmp(mime+6, "ogg") == 0 )
				return "ogg";
			else if( strcmp(mime, "audio/x-pn-realaudio") == 0 )
				return "ra";
			else if( strcmp(mime, "audio/aacp") == 0 )
				return "aac+";
			else if( strcmp(mime, "audio/eaacp") == 0 )
				return "eaac+";
			else if( strcmp(mime, "audio/amr") == 0 )
				return "amr";
			else if( strcmp(mime, "audio/mid") == 0 )
				return "mid";
			else if( strcmp(mime, "aaudio/mp2") == 0 )
				return "mp2";
			else if( strcmp(mime, "audio/aiff") == 0 )
				return "aif";
			else if( strcmp(mime, "audio/x-mpeg") == 0 )
				return "mpega";
			break;
		case 'v':
			if( strcmp(mime+6, "avi") == 0 )
				return "avi";
			else if( strcmp(mime+6, "divx") == 0 )
				return "avi";
			else if( strcmp(mime+6, "x-msvideo") == 0 )
				return "avi";
			else if( strcmp(mime+6, "mpeg") == 0 )
				return "mpg";
			else if( strcmp(mime+6, "mp4") == 0 )
				return "mp4";
			else if( strcmp(mime+6, "x-ms-wmv") == 0 )
				return "wmv";
			else if( strcmp(mime+6, "x-matroska") == 0 )
				return "mkv";
			else if( strcmp(mime+6, "x-mkv") == 0 )
				return "mkv";
			else if( strcmp(mime+6, "x-flv") == 0 )
				return "flv";
#ifdef BAIDU_DMS_OPT
                        else if( strcmp(mime+6, "x-pn-realvideo") == 0)
                            return "rm";
                        else if( strcmp(mime+6, "x-rm") == 0 )
                            return "rm";
                        else if( strcmp(mime+6, "x-rmvb") == 0 )
                            return "rmvb";
                       else if( strcmp(mime, "application/x-shockwave-flash") == 0 )
                            return "swf";
                        else if( strcmp(mime+6, "swf") == 0 )
                            return "swf";
                        else if( strcmp(mime+6, "video/mpeg4") == 0 )
                            return "mpeg4";
                        else if( strcmp(mime+6, "video/x-ms-wmx") == 0 )
                            return "wmx";
                        else if( strcmp(mime+6, "application/vnd.rn-realmedia-vbr") == 0 )
                            return "rmvb";
                        else if( strcmp(mime+6, "video/x-ms-wm") == 0 )
                            return "wm";
                        else if( strcmp(mime+6, "video/mpg") == 0 )
                            return "mpeg";
                        else if( strcmp(mime+6, "video/mpg") == 0 )
                            return "mpeg2";
                        else if( strcmp(mime+6, "video/quicktime") == 0 )
                             return "qt";
                        else if( strcmp(mime+6, "application/x-ms-wmz") == 0 )
                             return "wmz";
                        else if( strcmp(mime+6, "application/x-ms-wmd") == 0 )
                             return "wmd";
                        else if( strcmp(mime+6, "video/mp4") == 0 )
                             return "f4v";
                        else if( strcmp(mime+6, "application/x-troll-ts") == 0 )
                              return "ts";

#endif
                        else if( strcmp(mime+6, "vnd.dlna.mpeg-tts") == 0 )
				return "mpg";
			else if( strcmp(mime+6, "quicktime") == 0 )
				return "mov";
			else if( strcmp(mime+6, "3gpp") == 0 )
				return "3gp";
			else if( strncmp(mime+6, "x-tivo-mpeg", 11) == 0 )
				return "TiVo";
			break;
		case 'i':
			if( strcmp(mime+6, "jpeg") == 0 )
				return "jpg";
			else if( strcmp(mime+6, "png") == 0 )
				return "png";
#ifdef BAIDU_DMS_OPT
			else if( strcmp(mime+6, "gif") == 0)
				return "gif";
			else if( strcmp(mime+6, "png") == 0)
				return "png";
			else if( strcmp(mime+6, "ico") == 0)
				return "ico";
			else if( strcmp(mime+6, "pcb") == 0)
				return "pcb";
			else if( strcmp(mime+6, "pnm") == 0)
				return "pnm";
			else if( strcmp(mime+6, "ppm") == 0)
				return "ppm";
			else if( strcmp(mime+6, "qti") == 0)
				return "qti";
			else if( strcmp(mime+6, "qtf") == 0)
				return "qtf";
			else if( strcmp(mime+6, "qtif") == 0)
				return "qtif";
			else if( strcmp(mime+6, "tif") == 0)
				return "tif";
			else if( strcmp(mime+6, "tiff") == 0)
				return "tiff";
#endif
			break;
		default:
			break;
	}
	return "dat";
}

int
is_video(const char * file)
{
	return (ends_with(file, ".mpg") || ends_with(file, ".mpeg")  ||
		ends_with(file, ".avi") || ends_with(file, ".divx")  ||
		ends_with(file, ".asf") || ends_with(file, ".wmv")   ||
		ends_with(file, ".mp4") || ends_with(file, ".m4v")   ||
		ends_with(file, ".mts") || ends_with(file, ".m2ts")  ||
		ends_with(file, ".m2t") || ends_with(file, ".mkv")   ||
		ends_with(file, ".vob") || ends_with(file, ".ts")    ||
		ends_with(file, ".flv") || ends_with(file, ".xvid")  ||
#ifdef BAIDU_DMS_OPT
                ends_with(file, ".rm")  || ends_with(file, ".rmvb")  ||
                ends_with(file, ".mpeg4")  || ends_with(file, ".swf")  ||
                ends_with(file, ".wmx")  || ends_with(file, ".wm")  ||
                ends_with(file, ".mpeg")  || ends_with(file, ".mpeg2")  ||
                ends_with(file, ".mpga")  || ends_with(file, ".qt")  ||
                ends_with(file, ".wmz")  || ends_with(file, ".wmd")  ||
                ends_with(file, ".wmd")  || ends_with(file, ".f4v")  ||
                ends_with(file, ".ts")  || ends_with(file, ".wvx")  ||
#endif
#ifdef TIVO_SUPPORT
		ends_with(file, ".TiVo") ||
#endif
		ends_with(file, ".mov") || ends_with(file, ".3gp"));
}

int
is_audio(const char * file)
{
	return (ends_with(file, ".mp3") || ends_with(file, ".flac") ||
		ends_with(file, ".wma") || ends_with(file, ".asf")  ||
		ends_with(file, ".fla") || ends_with(file, ".flc")  ||
		ends_with(file, ".m4a") || ends_with(file, ".aac")  ||
		ends_with(file, ".mp4") || ends_with(file, ".m4p")  ||
		ends_with(file, ".pcm") || ends_with(file, ".3gp")  ||
#ifdef BAIDU_DMS_OPT
		ends_with(file, ".ra")  || ends_with(file, ".aac+") ||
		ends_with(file, ".eaac+")|| ends_with(file, ".amr") ||
		ends_with(file, ".mid") || ends_with(file, ".midi") ||
		ends_with(file, ".mp2") || ends_with(file, ".aif")  ||
		ends_with(file, ".mpega") || ends_with(file, ".ram")||
#endif
		ends_with(file, ".wav") || ends_with(file, ".ogg"));
}

int
is_image(const char * file)
{
#ifdef BAIDU_DMS_OPT
	return (ends_with(file, ".jpg") || ends_with(file, ".jpeg") ||
			ends_with(file, ".png") || ends_with(file,".gif")  ||
			ends_with(file, ".ico") || ends_with(file,".ief")  ||
			ends_with(file, ".ifm") || ends_with(file,".ifs")  ||
			ends_with(file, ".ppm") || ends_with(file,".qtif") ||
			ends_with(file, ".tif") || ends_with(file,".tiff") ||
			ends_with(file, ".pcd") || ends_with(file,".qti")  ||
			ends_with(file, ".qtf") || ends_with(file,".pnm")  ||
			ends_with(file, ".bmp") || ends_with(file, ".psd") ||
			ends_with(file, ".svg") || ends_with(file,".svgz") ||
			ends_with(file, ".cur") || ends_with(file,".jpe"));
#else
	return (ends_with(file, ".jpg") || ends_with(file, ".jpeg"));
#endif
}

int
is_playlist(const char * file)
{
	return (ends_with(file, ".m3u") || ends_with(file, ".pls"));
}

int
is_album_art(const char * name)
{
	struct album_art_name_s * album_art_name;

	/* Check if this file name matches one of the default album art names */
	for( album_art_name = album_art_names; album_art_name; album_art_name = album_art_name->next )
	{
		if( album_art_name->wildcard )
		{
			if( strncmp(album_art_name->name, name, strlen(album_art_name->name)) == 0 )
				break;
		}
		else
		{
			if( strcmp(album_art_name->name, name) == 0 )
				break;
		}
	}

	return (album_art_name ? 1 : 0);
}

int
resolve_unknown_type(const char * path, media_types dir_type)
{
	struct stat entry;
	unsigned char type = TYPE_UNKNOWN;
	char str_buf[PATH_MAX];
	ssize_t len;

	if( lstat(path, &entry) == 0 )
	{
		if( S_ISLNK(entry.st_mode) )
		{
			if( (len = readlink(path, str_buf, PATH_MAX-1)) > 0 )
			{
				str_buf[len] = '\0';
				//DEBUG DPRINTF(E_DEBUG, L_GENERAL, "Checking for recursive symbolic link: %s (%s)\n", path, str_buf);
				if( strncmp(path, str_buf, strlen(str_buf)) == 0 )
				{
					DPRINTF(E_DEBUG, L_GENERAL, "Ignoring recursive symbolic link: %s (%s)\n", path, str_buf);
					return type;
				}
			}
			stat(path, &entry);
		}

		if( S_ISDIR(entry.st_mode) )
		{
			type = TYPE_DIR;
		}
		else if( S_ISREG(entry.st_mode) )
		{
			switch( dir_type )
			{
				case ALL_MEDIA:
					if( is_image(path) ||
					    is_audio(path) ||
					    is_video(path) ||
					    is_playlist(path) )
						type = TYPE_FILE;
					break;
				case TYPE_AUDIO:
					if( is_audio(path) ||
					    is_playlist(path) )
						type = TYPE_FILE;
					break;
				case TYPE_VIDEO:
					if( is_video(path) )
						type = TYPE_FILE;
					break;
				case TYPE_IMAGES:
					if( is_image(path) )
						type = TYPE_FILE;
					break;
				default:
					break;
			}
		}
	}
	return type;
}

