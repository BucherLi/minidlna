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
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>

#include <libexif/exif-loader.h>
#include <jpeglib.h>
#include <setjmp.h>
#include "libav.h"

#include "upnpglobalvars.h"
#include "tagutils/tagutils.h"
#include "image_utils.h"
#include "upnpreplyparse.h"
#include "tivo_utils.h"
#include "metadata.h"
#include "albumart.h"
#include "utils.h"
#include "sql.h"
#include "log.h"

#define FLAG_TITLE	0x00000001
#define FLAG_ARTIST	0x00000002
#define FLAG_ALBUM	0x00000004
#define FLAG_GENRE	0x00000008
#define FLAG_COMMENT	0x00000010
#define FLAG_CREATOR	0x00000020
#define FLAG_DATE	0x00000040
#define FLAG_DLNA_PN	0x00000080
#define FLAG_MIME	0x00000100
#define FLAG_DURATION	0x00000200
#define FLAG_RESOLUTION	0x00000400
#define FLAG_BITRATE	0x00000800
#define FLAG_FREQUENCY	0x00001000
#define FLAG_BPS	0x00002000
#define FLAG_CHANNELS	0x00004000
#define FLAG_ROTATION	0x00008000

/* Audio profile flags */
enum audio_profiles {
	PROFILE_AUDIO_UNKNOWN,
	PROFILE_AUDIO_MP3,
	PROFILE_AUDIO_AC3,
	PROFILE_AUDIO_WMA_BASE,
	PROFILE_AUDIO_WMA_FULL,
	PROFILE_AUDIO_WMA_PRO,
	PROFILE_AUDIO_MP2,
	PROFILE_AUDIO_PCM,
	PROFILE_AUDIO_AAC,
	PROFILE_AUDIO_AAC_MULT5,
	PROFILE_AUDIO_AMR
};

static inline int
lav_open(AVFormatContext **ctx, const char *filename)
{
	int ret;
#if LIBAVFORMAT_VERSION_INT >= ((53<<16)+(17<<8)+0)
	ret = avformat_open_input(ctx, filename, NULL, NULL);
	if (ret == 0)
		avformat_find_stream_info(*ctx, NULL);
#else
	ret = av_open_input_file(ctx, filename, NULL, 0, NULL);
	if (ret == 0)
		av_find_stream_info(*ctx);
#endif
	return ret;
}

static inline void
lav_close(AVFormatContext *ctx)
{
#if LIBAVFORMAT_VERSION_INT >= ((53<<16)+(17<<8)+0)
	avformat_close_input(&ctx);
#else
	av_close_input_file(ctx);
#endif
}

#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
# if LIBAVUTIL_VERSION_INT < ((51<<16)+(5<<8)+0) && !defined(FF_API_OLD_METADATA2)
#define AV_DICT_IGNORE_SUFFIX AV_METADATA_IGNORE_SUFFIX
#define av_dict_get av_metadata_get
typedef AVMetadataTag AVDictionaryEntry;
# endif
#endif

/* This function shamelessly copied from libdlna */
#define MPEG_TS_SYNC_CODE 0x47
#define MPEG_TS_PACKET_LENGTH 188
#define MPEG_TS_PACKET_LENGTH_DLNA 192 /* prepends 4 bytes to TS packet */
int
dlna_timestamp_is_present(const char *filename, int *raw_packet_size)
{
	unsigned char buffer[3*MPEG_TS_PACKET_LENGTH_DLNA];
	int fd, i;

	/* read file header */
	fd = open(filename, O_RDONLY);
	if( fd < 0 )
		return 0;
	i = read(fd, buffer, MPEG_TS_PACKET_LENGTH_DLNA*3);
	close(fd);
	if( i < 0 )
		return 0;
	for( i = 0; i < MPEG_TS_PACKET_LENGTH_DLNA; i++ )
	{
		if( buffer[i] == MPEG_TS_SYNC_CODE )
		{
			if (buffer[i + MPEG_TS_PACKET_LENGTH_DLNA] == MPEG_TS_SYNC_CODE &&
			    buffer[i + MPEG_TS_PACKET_LENGTH_DLNA*2] == MPEG_TS_SYNC_CODE)
			{
			        *raw_packet_size = MPEG_TS_PACKET_LENGTH_DLNA;
				if (buffer[i+MPEG_TS_PACKET_LENGTH] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+1] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+2] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+3] == 0x00)
					return 0;
				else
					return 1;
			} else if (buffer[i + MPEG_TS_PACKET_LENGTH] == MPEG_TS_SYNC_CODE &&
				   buffer[i + MPEG_TS_PACKET_LENGTH*2] == MPEG_TS_SYNC_CODE) {
			    *raw_packet_size = MPEG_TS_PACKET_LENGTH;
			    return 0;
			}
		}
	}
	*raw_packet_size = 0;
	return 0;
}

void
check_for_captions(const char *path, int64_t detailID)
{
	char *file = malloc(MAXPATHLEN);
	char *id = NULL;

	sprintf(file, "%s", path);
	strip_ext(file);

	/* If we weren't given a detail ID, look for one. */
	if( !detailID )
	{
		id = sql_get_text_field(db, "SELECT ID from DETAILS where (PATH > '%q.' and PATH <= '%q.z')"
		                            " and MIME glob 'video/*' limit 1", file, file);
		if( id )
		{
			//DPRINTF(E_MAXDEBUG, L_METADATA, "New file %s looks like a caption file.\n", path);
			detailID = strtoll(id, NULL, 10);
		}
		else
		{
			//DPRINTF(E_MAXDEBUG, L_METADATA, "No file found for caption %s.\n", path);
			goto no_source_video;
		}
	}

	strcat(file, ".srt");
	if( access(file, R_OK) == 0 )
	{
		sql_exec(db, "INSERT into CAPTIONS"
		             " (ID, PATH) "
		             "VALUES"
		             " (%lld, %Q)", detailID, file);
	}
no_source_video:
	if( id )
		sqlite3_free(id);
	free(file);
}

void
parse_nfo(const char *path, metadata_t *m)
{
	FILE *nfo;
	char buf[65536];
	struct NameValueParserData xml;
	struct stat file;
	size_t nread;
	char *val, *val2;

	if( stat(path, &file) != 0 ||
	    file.st_size > 65536 )
	{
		DPRINTF(E_INFO, L_METADATA, "Not parsing very large .nfo file %s\n", path);
		return;
	}
	DPRINTF(E_DEBUG, L_METADATA, "Parsing .nfo file: %s\n", path);
	nfo = fopen(path, "r");
	if( !nfo )
		return;
	nread = fread(&buf, 1, sizeof(buf), nfo);
	
	ParseNameValue(buf, nread, &xml, 0);

	//printf("\ttype: %s\n", GetValueFromNameValueList(&xml, "rootElement"));
	val = GetValueFromNameValueList(&xml, "title");
	if( val )
	{
		char *esc_tag = unescape_tag(val, 1);
		val2 = GetValueFromNameValueList(&xml, "episodetitle");
		if( val2 ) {
			char *esc_tag2 = unescape_tag(val2, 1);
			xasprintf(&m->title, "%s - %s", esc_tag, esc_tag2);
			free(esc_tag2);
		} else {
			m->title = escape_tag(esc_tag, 1);
		}
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "plot");
	if( val ) {
		char *esc_tag = unescape_tag(val, 1);
		m->comment = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "capturedate");
	if( val ) {
		char *esc_tag = unescape_tag(val, 1);
		m->date = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "genre");
	if( val )
	{
		free(m->genre);
		char *esc_tag = unescape_tag(val, 1);
		m->genre = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	val = GetValueFromNameValueList(&xml, "mime");
	if( val )
	{
		free(m->mime);
		char *esc_tag = unescape_tag(val, 1);
		m->mime = escape_tag(esc_tag, 1);
		free(esc_tag);
	}

	ClearNameValueList(&xml);
	fclose(nfo);
}

void
free_metadata(metadata_t *m, uint32_t flags)
{
	if( flags & FLAG_TITLE )
		free(m->title);
	if( flags & FLAG_ARTIST )
		free(m->artist);
	if( flags & FLAG_ALBUM )
		free(m->album);
	if( flags & FLAG_GENRE )
		free(m->genre);
	if( flags & FLAG_CREATOR )
		free(m->creator);
	if( flags & FLAG_DATE )
		free(m->date);
	if( flags & FLAG_COMMENT )
		free(m->comment);
	if( flags & FLAG_DLNA_PN )
		free(m->dlna_pn);
	if( flags & FLAG_MIME )
		free(m->mime);
	if( flags & FLAG_DURATION )
		free(m->duration);
	if( flags & FLAG_RESOLUTION )
		free(m->resolution);
	if( flags & FLAG_BITRATE )
		free(m->bitrate);
	if( flags & FLAG_FREQUENCY )
		free(m->frequency);
	if( flags & FLAG_BPS )
		free(m->bps);
	if( flags & FLAG_CHANNELS )
		free(m->channels);
	if( flags & FLAG_ROTATION )
		free(m->rotation);
}

int64_t
GetFolderMetadata(const char *name, const char *path, const char *artist, const char *genre, int64_t album_art)
{
	int ret;

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (TITLE, PATH, CREATOR, ARTIST, GENRE, ALBUM_ART) "
	                   "VALUES"
	                   " ('%q', %Q, %Q, %Q, %Q, %lld);",
	                   name, path, artist, artist, genre, album_art);
	if( ret != SQLITE_OK )
		ret = 0;
	else
		ret = sqlite3_last_insert_rowid(db);

	return ret;
}
#ifdef NAS
int64_t
GetFolderMetadata2(const char *name, const char *path, const char *artist, const char *genre, int64_t album_art)
{
	int ret;

	ret = sql_exec(db2, "INSERT into DETAILS"
	                   " (TITLE, PATH, CREATOR, ARTIST, GENRE, ALBUM_ART) "
	                   "VALUES"
	                   " ('%q', %Q, %Q, %Q, %Q, %lld);",
	                   name, path, artist, artist, genre, album_art);
	if( ret != SQLITE_OK )
		ret = 0;
	else
		ret = sqlite3_last_insert_rowid(db2);

	return ret;
}
#endif
int64_t
GetAudioMetadata(const char *path, char *name)
{
#ifdef NAS
	int dir_count=0,num=0;
	char full_dir[64];
	snprintf(full_dir,sizeof(full_dir),"%s",path);
	printf("full_dir:%s\n",full_dir);
	while(full_dir[num] != '\0')
	{
		if(dir_count > 11){

			return 0;
		}

		if('/'== full_dir[num]){
			dir_count++;
		}
		num++;
	}
#endif
#ifdef BAIDU_DMS_OPT
	char full_name[64];
	snprintf(full_name,sizeof(full_name),"%s",name);
#endif
	char type[4];
	static char lang[6] = { '\0' };
	struct stat file;
	int64_t ret;
	char *esc_tag;
	int i;
	int64_t album_art = 0;
	struct song_metadata song;
	metadata_t m;
	uint32_t free_flags = FLAG_MIME|FLAG_DURATION|FLAG_DLNA_PN|FLAG_DATE;
	memset(&m, '\0', sizeof(metadata_t));

	if ( stat(path, &file) != 0 )
		return 0;

	strip_ext(name);
#ifdef BAIDU_DMS_OPT
	if( ends_with(path, ".mp3") || ends_with(path, ".flac") ||
		ends_with(path, ".wma") || ends_with(path, ".asf")  ||
		ends_with(path, ".fla") || ends_with(path, ".flc")  ||
		ends_with(path, ".m4a") || ends_with(path, ".aac")  ||
		ends_with(path, ".mp4") || ends_with(path, ".m4p")  ||
		ends_with(path, ".pcm") || ends_with(path, ".3gp")  )
  {
#endif
	if( ends_with(path, ".mp3") )
	{
		strcpy(type, "mp3");
		m.mime = strdup("audio/mpeg");
	}
	else if( ends_with(path, ".m4a") || ends_with(path, ".mp4") ||
	         ends_with(path, ".aac") || ends_with(path, ".m4p") )
	{
		strcpy(type, "aac");
		m.mime = strdup("audio/mp4");
	}
	else if( ends_with(path, ".3gp") )
	{
		strcpy(type, "aac");
		m.mime = strdup("audio/3gpp");
	}
	else if( ends_with(path, ".wma") || ends_with(path, ".asf") )
	{
		strcpy(type, "asf");
		m.mime = strdup("audio/x-ms-wma");
	}
	else if( ends_with(path, ".flac") || ends_with(path, ".fla") || ends_with(path, ".flc") )
	{
		strcpy(type, "flc");
		m.mime = strdup("audio/x-flac");
	}

	else if( ends_with(path, ".pcm") )
	{
		strcpy(type, "pcm");
		m.mime = strdup("audio/L16");
	}


	if( !(*lang) )
	{
		if( !getenv("LANG") )
			strcpy(lang, "en_US");
		else
			strncpyt(lang, getenv("LANG"), sizeof(lang));
	}

	if( readtags((char *)path, &song, &file, lang, type) != 0 )
	{
		DPRINTF(E_WARN, L_GENERAL, "Cannot extract tags from %s!\n", path);
        	freetags(&song);
		free_metadata(&m, free_flags);
		return 0;
	}

	if( song.dlna_pn )
		m.dlna_pn = strdup(song.dlna_pn);
	if( song.year )
		xasprintf(&m.date, "%04d-01-01", song.year);
	xasprintf(&m.duration, "%d:%02d:%02d.%03d",
	                      (song.song_length/3600000),
	                      (song.song_length/60000%60),
	                      (song.song_length/1000%60),
	                      (song.song_length%1000));
	if( song.title && *song.title )
	{
		m.title = trim(song.title);
		if( (esc_tag = escape_tag(m.title, 0)) )
		{
			free_flags |= FLAG_TITLE;
			m.title = esc_tag;
		}
	}
	else
	{
		m.title = name;
	}
	for( i=ROLE_START; i<N_ROLE; i++ )
	{
		if( song.contributor[i] && *song.contributor[i] )
		{
			m.creator = trim(song.contributor[i]);
			if( strlen(m.creator) > 48 )
			{
				m.creator = strdup("Various Artists");
				free_flags |= FLAG_CREATOR;
			}
			else if( (esc_tag = escape_tag(m.creator, 0)) )
			{
				m.creator = esc_tag;
				free_flags |= FLAG_CREATOR;
			}
			m.artist = m.creator;
			break;
		}
	}
	/* If there is a band associated with the album, use it for virtual containers. */
	if( (i != ROLE_BAND) && (i != ROLE_ALBUMARTIST) )
	{
	        if( song.contributor[ROLE_BAND] && *song.contributor[ROLE_BAND] )
		{
			i = ROLE_BAND;
			m.artist = trim(song.contributor[i]);
			if( strlen(m.artist) > 48 )
			{
				m.artist = strdup("Various Artists");
				free_flags |= FLAG_ARTIST;
			}
			else if( (esc_tag = escape_tag(m.artist, 0)) )
			{
				m.artist = esc_tag;
				free_flags |= FLAG_ARTIST;
			}
		}
	}
	if( song.album && *song.album )
	{
		m.album = trim(song.album);
		if( (esc_tag = escape_tag(m.album, 0)) )
		{
			free_flags |= FLAG_ALBUM;
			m.album = esc_tag;
		}
	}
	if( song.genre && *song.genre )
	{
		m.genre = trim(song.genre);
		if( (esc_tag = escape_tag(m.genre, 0)) )
		{
			free_flags |= FLAG_GENRE;
			m.genre = esc_tag;
		}
	}
	if( song.comment && *song.comment )
	{
		m.comment = trim(song.comment);
		if( (esc_tag = escape_tag(m.comment, 0)) )
		{
			free_flags |= FLAG_COMMENT;
			m.comment = esc_tag;
		}
	}
#ifdef BAIDU_DMS_OPT
	album_art = find_album_art(path, song.image, song.image_size);

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE,TYPE,TIMESTAMP_mtime,TIMESTAMP_ctime, DURATION, CHANNELS, BITRATE, SAMPLERATE, DATE,"
	                   "  TITLE, CREATOR, ARTIST, ALBUM, GENRE, COMMENT, DISC, TRACK, DLNA_PN, MIME, ALBUM_ART) "
	                   "VALUES"
	                   " (%Q, %lld,%Q, %ld,%ld,'%s', %d, %d, %d, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %d, %d, %Q, '%s', %lld);",
	                   path, (long long)file.st_size,"audio",file.st_mtime,file.st_ctime, m.duration, song.channels, song.bitrate, song.samplerate, m.date,
	                   full_name, m.creator, m.artist, m.album, m.genre, m.comment, song.disc, song.track,
	                   m.dlna_pn, song.mime?song.mime:m.mime, album_art);
#else
	album_art = find_album_art(path, song.image, song.image_size);

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE, TIMESTAMP, DURATION, CHANNELS, BITRATE, SAMPLERATE, DATE,"
	                   "  TITLE, CREATOR, ARTIST, ALBUM, GENRE, COMMENT, DISC, TRACK, DLNA_PN, MIME, ALBUM_ART) "
	                   "VALUES"
	                   " (%Q, %lld, %ld, '%s', %d, %d, %d, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %d, %d, %Q, '%s', %lld);",
	                   path, (long long)file.st_size, file.st_mtime, m.duration, song.channels, song.bitrate, song.samplerate, m.date,
	                   m.title, m.creator, m.artist, m.album, m.genre, m.comment, song.disc, song.track,
	                   m.dlna_pn, song.mime?song.mime:m.mime, album_art);
#endif
#ifdef BAIDU_DMS_OPT
  }
	else
  {
	if( ends_with(path, ".ra") )
	{
		strcpy(type, "ra");
		m.mime = strdup("audio/x-pn-realaudio");
	}
	else if( ends_with(path, ".ram") )
	{
		strcpy(type, "ra");
		m.mime = strdup("audio/x-pn-realaudio");
	}
	else if( ends_with(path, ".aac+") )
	{
		strcpy(type, "aac+");
		m.mime = strdup("audio/aacp");
	}
	else if( ends_with(path, ".eaac+") )
	{
		strcpy(type, "eaac+");
		m.mime = strdup("audio/eaacp");
	}
	else if( ends_with(path, ".amr") )
	{
		strcpy(type, "amr");
		m.mime = strdup("audio/amr");
	}
	else if( ends_with(path, ".mid") )
	{
		strcpy(type, "mid");
		m.mime = strdup("audio/mid");
	}
	else if( ends_with(path, ".midi") )
	{
		strcpy(type, "midi");
		m.mime = strdup("audio/mid");
	}
	else if( ends_with(path, ".mp2") )
	{
		strcpy(type, "mp2");
		m.mime = strdup("audio/mp2");
	}
	else if( ends_with(path, ".aif") )
	{
		strcpy(type, "aif");
		m.mime = strdup("audio/aiff");
	}
	else if( ends_with(path, ".mpega"))
	{
		strcpy(type, "mpega");
		m.mime = strdup("audio/x-mpeg");
	}
	else if( ends_with(path, ".wav") )
	{
		strcpy(type, "wav");
		m.mime = strdup("audio/x-wav");
	}
	else if( ends_with(path, ".ogg" ))
	{
		strcpy(type, "ogg");
		m.mime = strdup("audio/ogg");
	}
	else
		{
			DPRINTF(E_WARN, L_GENERAL, "Unhandled file extension on %s\n", path);
			return 0;
		}
	if( readtags((char *)path, &song, &file, lang, type) != 0 )
		{
			DPRINTF(E_WARN, L_GENERAL, "Cannot extract tags from %s!\n", path);
	        	freetags(&song);
			free_metadata(&m, free_flags);
			return 0;
		}
	ret = sql_exec(db, "INSERT into DETAILS"
		                   " (PATH, SIZE,TYPE,TIMESTAMP_mtime,TIMESTAMP_ctime, DURATION, CHANNELS, BITRATE, SAMPLERATE, DATE,"
		                   "  TITLE, CREATOR, ARTIST, ALBUM, GENRE, COMMENT, DISC, TRACK, DLNA_PN, MIME, ALBUM_ART) "
		                   "VALUES"
		                   " (%Q, %lld,%Q,%ld,%ld,'%s', %d, %d, %d, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %d, %d, %Q, '%s', %lld);",
		                   path, (long long)file.st_size,"audio",file.st_mtime,file.st_ctime, NULL, NULL, NULL,NULL,NULL,
		                   full_name,NULL, NULL, NULL,NULL,NULL,0,0,NULL,m.mime, album_art);
  }
#endif
	if( ret != SQLITE_OK )
	{
		fprintf(stderr, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
	}
        freetags(&song);
	free_metadata(&m, free_flags);

	return ret;
}

/* For libjpeg error handling */
jmp_buf setjmp_buffer;
static void
libjpeg_error_handler(j_common_ptr cinfo)
{
	cinfo->err->output_message (cinfo);
	longjmp(setjmp_buffer, 1);
	return;
}

int64_t
GetImageMetadata(const char *path, char *name)
{
#ifdef NAS
	int dir_count=0,num=0;
	char full_dir[64];
	snprintf(full_dir,sizeof(full_dir),"%s",path);
	printf("full_dir:%s\n",full_dir);
	while(full_dir[num] != '\0')
	{
		if(dir_count > 11){

			return 0;
		}

		if('/'== full_dir[num]){
			dir_count++;
		}
		num++;
	}
#endif
#ifdef BAIDU_DMS_OPT
	char full_name[64];
	snprintf(full_name,sizeof(full_name),"%s",name);
#endif
	ExifData *ed;
	ExifEntry *e = NULL;
	ExifLoader *l;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	FILE *infile;
	int width=0, height=0, thumb=0;
	char make[32], model[64] = {'\0'};
	char b[1024];
	struct stat file;
	int64_t ret;
	image_s *imsrc;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	memset(&m, '\0', sizeof(metadata_t));
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Parsing %s...\n", path);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	/* MIME hard-coded to JPEG for now, until we add PNG support */
#ifdef BAIDU_DMS_OPT
	if( ends_with(path, ".jpg")){
#endif
	m.mime = strdup("image/jpeg");

	l = exif_loader_new();
	exif_loader_write_file(l, path);
	ed = exif_loader_get_data(l);
	exif_loader_unref(l);
	if( !ed )
		goto no_exifdata;

	e = exif_content_get_entry (ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL);
	if( e || (e = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_DIGITIZED)) )
	{
		m.date = strdup(exif_entry_get_value(e, b, sizeof(b)));
		if( strlen(m.date) > 10 )
		{
			m.date[4] = '-';
			m.date[7] = '-';
			m.date[10] = 'T';
		}
		else {
			free(m.date);
			m.date = NULL;
		}
	}
	else {
		/* One last effort to get the date from XMP */
		image_get_jpeg_date_xmp(path, &m.date);
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * date: %s\n", m.date);

	e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MAKE);
	if( e )
	{
		strncpyt(make, exif_entry_get_value(e, b, sizeof(b)), sizeof(make));
		e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL);
		if( e )
		{
			strncpyt(model, exif_entry_get_value(e, b, sizeof(b)), sizeof(model));
			if( !strcasestr(model, make) )
				snprintf(model, sizeof(model), "%s %s", make, exif_entry_get_value(e, b, sizeof(b)));
			m.creator = escape_tag(trim(model), 1);
		}
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * model: %s\n", model);

	e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
	if( e )
	{
		int rotate;
		switch( exif_get_short(e->data, exif_data_get_byte_order(ed)) )
		{
		case 3:
			rotate = 180;
			break;
		case 6:
			rotate = 90;
			break;
		case 8:
			rotate = 270;
			break;
		default:
			rotate = 0;
			break;
		}
		if( rotate )
			xasprintf(&m.rotation, "%d", rotate);
	}

	if( ed->size )
	{
		/* We might need to verify that the thumbnail is 160x160 or smaller */
		if( ed->size > 12000 )
		{
			imsrc = image_new_from_jpeg(NULL, 0, (char *)ed->data, ed->size, 1, ROTATE_NONE);
			if( imsrc )
			{
 				if( (imsrc->width <= 160) && (imsrc->height <= 160) )
					thumb = 1;
				image_free(imsrc);
			}
		}
		else
			thumb = 1;
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * thumbnail: %d\n", thumb);

	exif_data_unref(ed);

no_exifdata:
	/* If SOF parsing fails, then fall through to reading the JPEG data with libjpeg to get the resolution */
	if( image_get_jpeg_resolution(path, &width, &height) != 0 || !width || !height )
	{
		infile = fopen(path, "r");
		if( infile )
		{
			cinfo.err = jpeg_std_error(&jerr);
			jerr.error_exit = libjpeg_error_handler;
			jpeg_create_decompress(&cinfo);
			if( setjmp(setjmp_buffer) )
				goto error;
			jpeg_stdio_src(&cinfo, infile);
			jpeg_read_header(&cinfo, TRUE);
			jpeg_start_decompress(&cinfo);
			width = cinfo.output_width;
			height = cinfo.output_height;
			error:
			jpeg_destroy_decompress(&cinfo);
			fclose(infile);
		}
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * resolution: %dx%d\n", width, height);

	if( !width || !height )
	{
		free_metadata(&m, free_flags);
		return 0;
	}
	if( width <= 640 && height <= 480 )
		m.dlna_pn = strdup("JPEG_SM");
	else if( width <= 1024 && height <= 768 )
		m.dlna_pn = strdup("JPEG_MED");
	else if( (width <= 4096 && height <= 4096) || !GETFLAG(DLNA_STRICT_MASK) )
		m.dlna_pn = strdup("JPEG_LRG");
	xasprintf(&m.resolution, "%dx%d", width, height);
#ifdef BAIDU_DMS_OPT
	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, TITLE, SIZE, TYPE,TIMESTAMP_mtime,TIMESTAMP_ctime, DATE, RESOLUTION,"
	                    " ROTATION, THUMBNAIL, CREATOR, DLNA_PN, MIME) "
	                   "VALUES"
	                   " (%Q, '%q', %lld,%Q ,%ld,%ld, %Q, %Q, %Q, %d, %Q, %Q, %Q);",
	                   path, full_name, (long long)file.st_size,"image",file.st_mtime,file.st_ctime ,m.date, m.resolution,
                    m.rotation, thumb, m.creator, m.dlna_pn, m.mime);
#else
	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, TITLE, SIZE, TIMESTAMP, DATE, RESOLUTION,"
	                    " ROTATION, THUMBNAIL, CREATOR, DLNA_PN, MIME) "
	                   "VALUES"
	                   " (%Q, '%q', %lld, %ld, %Q, %Q, %Q, %d, %Q, %Q, %Q);",
	                   path, name, (long long)file.st_size, file.st_mtime, m.date, m.resolution,
                    m.rotation, thumb, m.creator, m.dlna_pn, m.mime);
#endif
#ifdef BAIDU_DMS_OPT
	}
	else
	{
		if(ends_with(path, ".png") )
		 {
			m.mime = strdup("image/png");

		  }
		else if(ends_with(path, ".gif"))
		{
			m.mime = strdup("image/gif");

		}
		else if(ends_with(path, ".jpeg"))
		{
			m.mime = strdup("image/jpg");

		}
		else if(ends_with(path, ".tif"))
		{
			m.mime = strdup("image/tiff");

		}
		else if(ends_with(path, ".tiff"))
					{
						m.mime = strdup("image/tiff");

					}
		else if(ends_with(path, ".ico"))
				{
					m.mime = strdup("image/x-icon");

				}
		else if(ends_with(path, ".ief"))
				{
					m.mime = strdup("image/ief");

				}
		else if(ends_with(path, ".ifm"))
				{
					m.mime = strdup("image/gif");

				}
		else if(ends_with(path, ".ifs"))
				{
					m.mime = strdup("image/ifs");

				}
		else if(ends_with(path, ".qti"))
				{
					m.mime = strdup("image/x-quicktime");

				}
		else if(ends_with(path, ".qtif"))
				{
					m.mime = strdup("image/x-quicktime");

				}
		else if(ends_with(path, ".pnm"))
				{
					m.mime = strdup("image/x-portable-anymap");

				}
		else if(ends_with(path, ".ppm"))
				{
					m.mime = strdup("image/x-portable-pixmap");

				}
		else if(ends_with(path, ".bmp"))
				{
					m.mime = strdup("image/bmp");

				}
		else if(ends_with(path, ".cur"))
				{
					m.mime = strdup("application/octet-stream");

				}
		else if(ends_with(path, ".jpe"))
				{
					m.mime = strdup("image/jpeg");

				}
		else if(ends_with(path, ".psd"))
				{
					m.mime = strdup("application/octet-stream ");

				}
		else if(ends_with(path, ".svg"))
				{
					m.mime = strdup("image/image/svg-xml");

				}
		else if(ends_with(path, ".svgz"))
				{
					m.mime = strdup("image/svg+xml");

				}
		else
		{
			DPRINTF(E_WARN, L_GENERAL, "Unhandled file extension on %s\n", path);
			return 0;
		}


		ret = sql_exec(db, "INSERT into DETAILS"
		                   " (PATH, TITLE, SIZE,TYPE, TIMESTAMP_mtime,TIMESTAMP_ctime, DATE, RESOLUTION,"
		                    " ROTATION, THUMBNAIL, CREATOR, DLNA_PN, MIME) "
		                   "VALUES"
		                   " (%Q, '%q', %lld,%Q, %ld,%ld, %Q, %Q, %Q, %d, %Q, %Q, %Q);",
		                   path, full_name, (long long)file.st_size,"image",file.st_mtime,file.st_ctime,NULL, "10x10",
		                   NULL, 0, NULL,NULL, m.mime);
	}
#endif
 
	if( ret != SQLITE_OK )
	{
		fprintf(stderr, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
	}
	free_metadata(&m, free_flags);

	return ret;
}

int64_t
GetVideoMetadata(const char *path, char *name)
{
#ifdef NAS
	int dir_count=0,num=0;
	char full_dir[64];
	snprintf(full_dir,sizeof(full_dir),"%s",path);
	printf("full_dir:%s\n",full_dir);
	while(full_dir[num] != '\0')
	{
		if(dir_count > 11){

			return 0;
		}

		if('/'== full_dir[num]){
			dir_count++;
		}
		num++;
	}
#endif

#ifdef BAIDU_NO_STRIP_VIDEO
#ifdef BAIDU_DMS_OPT
	char full_name[64];
	snprintf(full_name,sizeof(full_name),"%s",name);
#endif
	struct stat file;
	int ret, i=0;
	struct tm *modtime;
	AVFormatContext *ctx = NULL;
	AVCodecContext *ac = NULL, *vc = NULL;
	int audio_stream = -1, video_stream = -1;
	enum audio_profiles audio_profile = PROFILE_AUDIO_UNKNOWN;
	char fourcc[4];
	int64_t album_art = 0;
	char nfo[MAXPATHLEN], *ext;
	struct song_metadata video;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	char *path_cpy, *basepath;
	memset(&m, '\0', sizeof(m));
	memset(&video, '\0', sizeof(video));

	//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Parsing video %s...\n", name);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	ret = lav_open(&ctx, path);
	if( ret != 0 )
	{
		char err[128];
		av_strerror(ret, err, sizeof(err));
		DPRINTF(E_WARN, L_METADATA, "Opening %s failed! [%s]\n", path, err);
		return 0;
	}
	//dump_format(ctx, 0, NULL, 0);
	for( i=0; i<ctx->nb_streams; i++)
	{
		if( audio_stream == -1 &&
		    ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO )
		{
			audio_stream = i;
			ac = ctx->streams[audio_stream]->codec;
			continue;
		}
		else if( video_stream == -1 &&
			 ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO )
		{
			video_stream = i;
			vc = ctx->streams[video_stream]->codec;
			continue;
		}
	}
	path_cpy = strdup(path);
	basepath = basename(path_cpy);
	if( !vc )
	{
		/* This must not be a video file. */
		lav_close(ctx);
		if( !is_audio(path) )
			DPRINTF(E_DEBUG, L_METADATA, "File %s does not contain a video stream.\n", basepath);
		free(path_cpy);
		return 0;
	}

	if( ac )
	{
		aac_object_type_t aac_type = AAC_INVALID;
		switch( ac->codec_id )
		{
			case CODEC_ID_MP3:
				audio_profile = PROFILE_AUDIO_MP3;
				break;
			case CODEC_ID_AAC:
				if( !ac->extradata_size ||
				    !ac->extradata )
				{
					DPRINTF(E_DEBUG, L_METADATA, "No AAC type\n");
				}
				else
				{
					uint8_t data;
					memcpy(&data, ac->extradata, 1);
					aac_type = data >> 3;
				}
				switch( aac_type )
				{
					/* AAC Low Complexity variants */
					case AAC_LC:
					case AAC_LC_ER:
						if( ac->sample_rate < 8000 ||
						    ac->sample_rate > 48000 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported AAC: sample rate is not 8000 < %d < 48000\n",
								ac->sample_rate);
							break;
						}
						/* AAC @ Level 1/2 */
						if( ac->channels <= 2 &&
						    ac->bit_rate <= 576000 )
							audio_profile = PROFILE_AUDIO_AAC;
						else if( ac->channels <= 6 &&
							 ac->bit_rate <= 1440000 )
							audio_profile = PROFILE_AUDIO_AAC_MULT5;
						else
							DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC: %d channels, %d bitrate\n",
								ac->channels,
								ac->bit_rate);
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC type [%d]\n", aac_type);
						break;
				}
				break;
			case CODEC_ID_AC3:
			case CODEC_ID_DTS:
				audio_profile = PROFILE_AUDIO_AC3;
				break;
			case CODEC_ID_WMAV1:
			case CODEC_ID_WMAV2:
				/* WMA Baseline: stereo, up to 48 KHz, up to 192,999 bps */
				if ( ac->bit_rate <= 193000 )
					audio_profile = PROFILE_AUDIO_WMA_BASE;
				/* WMA Full: stereo, up to 48 KHz, up to 385 Kbps */
				else if ( ac->bit_rate <= 385000 )
					audio_profile = PROFILE_AUDIO_WMA_FULL;
				break;
			#if LIBAVCODEC_VERSION_INT > ((51<<16)+(50<<8)+1)
			case CODEC_ID_WMAPRO:
				audio_profile = PROFILE_AUDIO_WMA_PRO;
				break;
			#endif
			case CODEC_ID_MP2:
				audio_profile = PROFILE_AUDIO_MP2;
				break;
			case CODEC_ID_AMR_NB:
				audio_profile = PROFILE_AUDIO_AMR;
				break;
			default:
				if( (ac->codec_id >= CODEC_ID_PCM_S16LE) &&
				    (ac->codec_id < CODEC_ID_ADPCM_IMA_QT) )
					audio_profile = PROFILE_AUDIO_PCM;
				else
					DPRINTF(E_DEBUG, L_METADATA, "Unhandled audio codec [0x%X]\n", ac->codec_id);
				break;
		}
		xasprintf(&m.frequency, "%u", ac->sample_rate);
		#if LIBAVCODEC_VERSION_INT < (52<<16)
		xasprintf(&m.bps, "%u", ac->bits_per_sample);
		#else
		xasprintf(&m.bps, "%u", ac->bits_per_coded_sample);
		#endif
		xasprintf(&m.channels, "%u", ac->channels);
	}
	if( vc )
	{
		int off;
		int duration, hours, min, sec, ms;
		ts_timestamp_t ts_timestamp = NONE;
		DPRINTF(E_DEBUG, L_METADATA, "Container: '%s' [%s]\n", ctx->iformat->name, basepath);
		xasprintf(&m.resolution, "%dx%d", vc->width, vc->height);
		if( ctx->bit_rate > 8 )
			xasprintf(&m.bitrate, "%u", ctx->bit_rate / 8);
		if( ctx->duration > 0 ) {
			duration = (int)(ctx->duration / AV_TIME_BASE);
			hours = (int)(duration / 3600);
			min = (int)(duration / 60 % 60);
			sec = (int)(duration % 60);
			ms = (int)(ctx->duration / (AV_TIME_BASE/1000) % 1000);
			xasprintf(&m.duration, "%d:%02d:%02d.%03d", hours, min, sec, ms);
		}

		/* NOTE: The DLNA spec only provides for ASF (WMV), TS, PS, and MP4 containers.
		 * Skip DLNA parsing for everything else. */
		if( strcmp(ctx->iformat->name, "avi") == 0 )
		{
			xasprintf(&m.mime, "video/x-msvideo");
			if( vc->codec_id == CODEC_ID_MPEG4 )
			{
        			fourcc[0] = vc->codec_tag     & 0xff;
			        fourcc[1] = vc->codec_tag>>8  & 0xff;
		        	fourcc[2] = vc->codec_tag>>16 & 0xff;
			        fourcc[3] = vc->codec_tag>>24 & 0xff;
				if( memcmp(fourcc, "XVID", 4) == 0 ||
				    memcmp(fourcc, "DX50", 4) == 0 ||
				    memcmp(fourcc, "DIVX", 4) == 0 )
					xasprintf(&m.creator, "DiVX");
			}
		}
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 &&
		         ends_with(path, ".mov") )
			xasprintf(&m.mime, "video/quicktime");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
#ifdef BAIDU_DMS_OPT
                else if( strcmp(ctx->iformat->name, "rm") == 0 )
                        xasprintf(&m.mime, "video/x-pn-realvideo");
                else if(ends_with(path, ".swf") )
				 {
					m.mime = strdup("application/x-shockwave-flash");

				  }
                else if( strcmp(ctx->iformat->name, "mpeg4") == 0 )
                        xasprintf(&m.mime, "video/mpeg4");
                else if( strcmp(ctx->iformat->name, ".wmx") == 0 )
                        xasprintf(&m.mime, "video/x-ms-wmx");
                else if( strcmp(ctx->iformat->name, "rmvb") == 0 )
                        xasprintf(&m.mime, "application/vnd.rn-realmedia-vbr");
                else if( strcmp(ctx->iformat->name, ".wm") == 0 )
                        xasprintf(&m.mime, "video/x-ms-wm");
                else if( strcmp(ctx->iformat->name, ".mpeg") == 0 )
                        xasprintf(&m.mime, "video/mpg");
                else if( strcmp(ctx->iformat->name, ".mpeg2") == 0 )
                        xasprintf(&m.mime, "video/mpg");
                else if( strcmp(ctx->iformat->name, ".mpga") == 0 )
                        xasprintf(&m.mime, "audio/rn-mpeg");
                else if( strcmp(ctx->iformat->name, ".qt") == 0 )
                        xasprintf(&m.mime, "video/quicktime");
                else if( strcmp(ctx->iformat->name, ".wmz") == 0 )
                        xasprintf(&m.mime, "application/x-ms-wmz");
                else if( strcmp(ctx->iformat->name, ".wmd") == 0 )
                        xasprintf(&m.mime, "application/x-ms-wmd");
                else if( strcmp(ctx->iformat->name, ".f4v") == 0 )
                        xasprintf(&m.mime, "video/mp4");
                else if( strcmp(ctx->iformat->name, ".ts") == 0 )
                        xasprintf(&m.mime, "application/x-troll-ts");
                else if( strcmp(ctx->iformat->name, ".wvx") == 0 )
                        xasprintf(&m.mime, "video/x-ms-wvx");
#endif
		if( m.mime )
			goto video_no_dlna;

		switch( vc->codec_id )
		{
			case CODEC_ID_MPEG1VIDEO:
				if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					if( (vc->width  == 352) &&
					    (vc->height <= 288) )
					{
						m.dlna_pn = strdup("MPEG1");
					}
					xasprintf(&m.mime, "video/mpeg");
				}
				break;
			case CODEC_ID_MPEG2VIDEO:
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "MPEG_");
				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 TS packet size %d\n",
						video_stream, basepath, m.resolution, raw_packet_size);
					off += sprintf(m.dlna_pn+off, "TS_");
					if( (vc->width  >= 1280) &&
					    (vc->height >= 720) )
					{
						off += sprintf(m.dlna_pn+off, "HD_NA");
					}
					else
					{
						off += sprintf(m.dlna_pn+off, "SD_");
						if( (vc->height == 576) ||
						    (vc->height == 288) )
							off += sprintf(m.dlna_pn+off, "EU");
						else
							off += sprintf(m.dlna_pn+off, "NA");
					}
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if (dlna_ts_present)
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							xasprintf(&m.mime, "video/mpeg");
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 PS\n",
						video_stream, basepath, m.resolution);
					off += sprintf(m.dlna_pn+off, "PS_");
					if( (vc->height == 576) ||
					    (vc->height == 288) )
						off += sprintf(m.dlna_pn+off, "PAL");
					else
						off += sprintf(m.dlna_pn+off, "NTSC");
					xasprintf(&m.mime, "video/mpeg");
				}
				else
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s [%s] is %s non-DLNA MPEG2\n",
						video_stream, basepath, ctx->iformat->name, m.resolution);
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				break;
			case CODEC_ID_H264:
				m.dlna_pn = malloc(128);
				off = sprintf(m.dlna_pn, "AVC_");

				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					AVRational display_aspect_ratio;
					int fps, interlaced;
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);

					off += sprintf(m.dlna_pn+off, "TS_");
					if (vc->sample_aspect_ratio.num) {
						av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
						          vc->width  * vc->sample_aspect_ratio.num,
						          vc->height * vc->sample_aspect_ratio.den,
						          1024*1024);
					}
					if (ctx->streams[video_stream]->r_frame_rate.den)
						fps = ctx->streams[video_stream]->r_frame_rate.num / ctx->streams[video_stream]->r_frame_rate.den;
					else
						fps = 0;
					interlaced = vc->time_base.den ? (ctx->streams[video_stream]->r_frame_rate.num / vc->time_base.den) : 0;
					if( ((((vc->width == 1920 || vc->width == 1440) && vc->height == 1080) ||
					      (vc->width == 720 && vc->height == 480)) && fps == 59 && interlaced) ||
					    ((vc->width == 1280 && vc->height == 720) && fps == 59 && !interlaced) )
					{
						if( (vc->profile == FF_PROFILE_H264_MAIN || vc->profile == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_60_");
							vc->profile = FF_PROFILE_SKIP;
						}
					}
					else if( ((vc->width == 1920 && vc->height == 1080) ||
					          (vc->width == 1440 && vc->height == 1080) ||
					          (vc->width == 1280 && vc->height ==  720) ||
					          (vc->width ==  720 && vc->height ==  576)) &&
					          interlaced && fps == 50 )
					{
						if( (vc->profile == FF_PROFILE_H264_MAIN || vc->profile == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_50_");
							vc->profile = FF_PROFILE_SKIP;
						}
					}
					switch( vc->profile )
					{
						case FF_PROFILE_H264_BASELINE:
						case FF_PROFILE_H264_CONSTRAINED_BASELINE:
							off += sprintf(m.dlna_pn+off, "BL_");
							if( vc->width  <= 352 &&
							    vc->height <= 288 &&
							    vc->bit_rate <= 384000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF15_");
								break;
							}
							else if( vc->width  <= 352 &&
							         vc->height <= 288 &&
							         vc->bit_rate <= 3000000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF30_");
								break;
							}
							/* Fall back to Main Profile if it doesn't match a Baseline DLNA profile. */
							else
								off -= 3;
						default:
						case FF_PROFILE_H264_MAIN:
							off += sprintf(m.dlna_pn+off, "MP_");
							if( vc->profile != FF_PROFILE_H264_BASELINE &&
							    vc->profile != FF_PROFILE_H264_CONSTRAINED_BASELINE &&
							    vc->profile != FF_PROFILE_H264_MAIN )
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unknown AVC profile %d; assuming MP. [%s]\n",
									vc->profile, basepath);
							}
							if( vc->width  <= 720 &&
							    vc->height <= 576 &&
							    vc->bit_rate <= 10000000 )
							{
								off += sprintf(m.dlna_pn+off, "SD_");
							}
							else if( vc->width  <= 1920 &&
							         vc->height <= 1152 &&
							         vc->bit_rate <= 20000000 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%s, %dx%d, %dbps : %s]\n",
									m.dlna_pn, vc->width, vc->height, vc->bit_rate, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_H264_HIGH:
							off += sprintf(m.dlna_pn+off, "HP_");
							if( vc->width  <= 1920 &&
							    vc->height <= 1152 &&
							    vc->bit_rate <= 30000000 &&
							    audio_profile == PROFILE_AUDIO_AC3 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 HP video profile! [%dbps, %d audio : %s]\n",
									vc->bit_rate, audio_profile, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_SKIP:
							break;
					}
					if( !m.dlna_pn )
						break;
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							break;
						case PROFILE_AUDIO_AC3:
							off += sprintf(m.dlna_pn+off, "AC3");
							break;
						case PROFILE_AUDIO_AAC:
						case PROFILE_AUDIO_AAC_MULT5:
							off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file [%s]\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
					if( !m.dlna_pn )
						break;
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if( vc->profile == FF_PROFILE_H264_HIGH ||
						    dlna_ts_present )
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					off += sprintf(m.dlna_pn+off, "MP4_");

					switch( vc->profile ) {
					case FF_PROFILE_H264_BASELINE:
					case FF_PROFILE_H264_CONSTRAINED_BASELINE:
						if( vc->width  <= 352 &&
						    vc->height <= 288 )
						{
							if( ctx->bit_rate < 600000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF15_");
							else if( ctx->bit_rate < 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF30_");
							else
								goto mp4_mp_fallback;

							if( audio_profile == PROFILE_AUDIO_AMR )
							{
								off += sprintf(m.dlna_pn+off, "AMR");
							}
							else if( audio_profile == PROFILE_AUDIO_AAC )
							{
								off += sprintf(m.dlna_pn+off, "AAC_");
								if( ctx->bit_rate < 520000 )
								{
									off += sprintf(m.dlna_pn+off, "520");
								}
								else if( ctx->bit_rate < 940000 )
								{
									off += sprintf(m.dlna_pn+off, "940");
								}
								else
								{
									off -= 13;
									goto mp4_mp_fallback;
								}
							}
							else
							{
								off -= 9;
								goto mp4_mp_fallback;
							}
						}
						else if( vc->width  <= 720 &&
						         vc->height <= 576 )
						{
							if( vc->level == 30 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_L3L_SD_AAC");
							else if( vc->level <= 31 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else if( vc->width  <= 1280 &&
						         vc->height <= 720 )
						{
							if( vc->level <= 31 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else if( vc->level <= 32 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 21000000 )
								off += sprintf(m.dlna_pn+off, "BL_L32_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else
							goto mp4_mp_fallback;
						break;
					case FF_PROFILE_H264_MAIN:
					mp4_mp_fallback:
						off += sprintf(m.dlna_pn+off, "MP_");
						/* AVC MP4 SD profiles - 10 Mbps max */
						if( vc->width  <= 720 &&
						    vc->height <= 576 &&
						    vc->bit_rate <= 10000000 )
						{
							sprintf(m.dlna_pn+off, "SD_");
							if( audio_profile == PROFILE_AUDIO_AC3 )
								off += sprintf(m.dlna_pn+off, "AC3");
							else if( audio_profile == PROFILE_AUDIO_AAC ||
							         audio_profile == PROFILE_AUDIO_AAC_MULT5 )
								off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							else if( audio_profile == PROFILE_AUDIO_MP3 )
								off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							else
								m.dlna_pn[10] = '\0';
						}
						else if( vc->width  <= 1280 &&
						         vc->height <= 720 &&
						         vc->bit_rate <= 15000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_720p_AAC");
						}
						else if( vc->width  <= 1920 &&
						         vc->height <= 1080 &&
						         vc->bit_rate <= 21000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_1080i_AAC");
						}
						if( strlen(m.dlna_pn) <= 11 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file %s\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
						break;
					case FF_PROFILE_H264_HIGH:
						if( vc->width  <= 1920 &&
						    vc->height <= 1080 &&
						    vc->bit_rate <= 25000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HP_HD_AAC");
						}
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "AVC profile [%d] not recognized for file %s\n",
							vc->profile, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
						break;
					}
				}
				else
				{
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is h.264\n", video_stream, basepath);
				break;
			case CODEC_ID_MPEG4:
        			fourcc[0] = vc->codec_tag     & 0xff;
			        fourcc[1] = vc->codec_tag>>8  & 0xff;
			        fourcc[2] = vc->codec_tag>>16 & 0xff;
			        fourcc[3] = vc->codec_tag>>24 & 0xff;
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is MPEG4 [%c%c%c%c/0x%X]\n",
					video_stream, basepath,
					isprint(fourcc[0]) ? fourcc[0] : '_',
					isprint(fourcc[1]) ? fourcc[1] : '_',
					isprint(fourcc[2]) ? fourcc[2] : '_',
					isprint(fourcc[3]) ? fourcc[3] : '_',
					vc->codec_tag);

				if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					m.dlna_pn = malloc(128);
					off = sprintf(m.dlna_pn, "MPEG4_P2_");

					if( ends_with(path, ".3gp") )
					{
						xasprintf(&m.mime, "video/3gpp");
						switch( audio_profile )
						{
							case PROFILE_AUDIO_AAC:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AAC");
								break;
							case PROFILE_AUDIO_AMR:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AMR");
								break;
							default:
								DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for MPEG4-P2 3GP/0x%X file %s\n",
								        ac->codec_id, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
								break;
						}
					}
					else
					{
						if( ctx->bit_rate <= 1000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_ASP_AAC");
						}
						else if( ctx->bit_rate <= 4000000 &&
						         vc->width  <= 640 &&
						         vc->height <= 480 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_SP_VGA_AAC");
						}
						else
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%dx%d, %dbps]\n",
								vc->width,
								vc->height,
								ctx->bit_rate);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
					}
				}
				break;
			case CODEC_ID_WMV3:
				/* I'm not 100% sure this is correct, but it works on everything I could get my hands on */
				if( vc->extradata_size > 0 )
				{
					if( !((vc->extradata[0] >> 3) & 1) )
						vc->level = 0;
					if( !((vc->extradata[0] >> 6) & 1) )
						vc->profile = 0;
				}
			case CODEC_ID_VC1:
				if( strcmp(ctx->iformat->name, "asf") != 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Skipping DLNA parsing for non-ASF VC1 file %s\n", path);
					break;
				}
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "WMV");
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is VC1\n", video_stream, basepath);
				xasprintf(&m.mime, "video/x-ms-wmv");
				if( (vc->width  <= 176) &&
				    (vc->height <= 144) &&
				    (vc->level == 0) )
				{
					off += sprintf(m.dlna_pn+off, "SPLL_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPLL/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (vc->width  <= 352) &&
				         (vc->height <= 288) &&
				         (vc->profile == 0) &&
				         (ctx->bit_rate/8 <= 384000) )
				{
					off += sprintf(m.dlna_pn+off, "SPML_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPML/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (vc->width  <= 720) &&
				         (vc->height <= 576) &&
				         (ctx->bit_rate/8 <= 10000000) )
				{
					off += sprintf(m.dlna_pn+off, "MED_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVMED/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (vc->width  <= 1920) &&
				         (vc->height <= 1080) &&
				         (ctx->bit_rate/8 <= 20000000) )
				{
					off += sprintf(m.dlna_pn+off, "HIGH_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVHIGH/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				break;
			case CODEC_ID_MSMPEG4V3:
				xasprintf(&m.mime, "video/x-msvideo");
			default:
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s [type %d]\n",
					video_stream, basepath, m.resolution, vc->codec_id);
				break;
		}
	}

	if( strcmp(ctx->iformat->name, "asf") == 0 )
	{
		if( readtags((char *)path, &video, &file, "en_US", "asf") == 0 )
		{
			if( video.title && *video.title )
			{
				m.title = escape_tag(trim(video.title), 1);
			}
			if( video.genre && *video.genre )
			{
				m.genre = escape_tag(trim(video.genre), 1);
			}
			if( video.contributor[ROLE_TRACKARTIST] && *video.contributor[ROLE_TRACKARTIST] )
			{
				m.artist = escape_tag(trim(video.contributor[ROLE_TRACKARTIST]), 1);
			}
			if( video.contributor[ROLE_ALBUMARTIST] && *video.contributor[ROLE_ALBUMARTIST] )
			{
				m.creator = escape_tag(trim(video.contributor[ROLE_ALBUMARTIST]), 1);
			}
			else
			{
				m.creator = m.artist;
				free_flags &= ~FLAG_CREATOR;
			}
		}
	}
	#ifndef NETGEAR
	#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
	else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
	{
		if( ctx->metadata )
		{
			AVDictionaryEntry *tag = NULL;

			//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Metadata:\n");
			while( (tag = av_dict_get(ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) )
			{
				//DEBUG DPRINTF(E_DEBUG, L_METADATA, "  %-16s: %s\n", tag->key, tag->value);
				if( strcmp(tag->key, "title") == 0 )
					m.title = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "genre") == 0 )
					m.genre = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "artist") == 0 )
					m.artist = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "comment") == 0 )
					m.comment = escape_tag(trim(tag->value), 1);
			}
		}
	}
	#endif
	#endif
video_no_dlna:

#ifdef TIVO_SUPPORT
	if( ends_with(path, ".TiVo") && is_tivo_file(path) )
	{
		if( m.dlna_pn )
		{
			free(m.dlna_pn);
			m.dlna_pn = NULL;
		}
		m.mime = realloc(m.mime, 21);
		strcpy(m.mime, "video/x-tivo-mpeg");
	}
#endif

	strcpy(nfo, path);
	ext = strrchr(nfo, '.');
	if( ext )
	{
		strcpy(ext+1, "nfo");
		if( access(nfo, F_OK) == 0 )
		{
			parse_nfo(nfo, &m);
		}
	}

	if( !m.mime )
	{
		if( strcmp(ctx->iformat->name, "avi") == 0 )
			xasprintf(&m.mime, "video/x-msvideo");
		else if( strncmp(ctx->iformat->name, "mpeg", 4) == 0 )
			xasprintf(&m.mime, "video/mpeg");
		else if( strcmp(ctx->iformat->name, "asf") == 0 )
			xasprintf(&m.mime, "video/x-ms-wmv");
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
			if( ends_with(path, ".mov") )
				xasprintf(&m.mime, "video/quicktime");
			else
				xasprintf(&m.mime, "video/mp4");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
#ifdef BAIDU_DMS_OPT
                else if( strcmp(ctx->iformat->name, "rm") == 0 )
                        xasprintf(&m.mime, "video/x-pn-realvideo");
                else if( strcmp(ctx->iformat->name, "mpeg4") == 0 )
                        xasprintf(&m.mime, "video/mpeg4");
                else if( strcmp(ctx->iformat->name, ".wmx") == 0 )
                        xasprintf(&m.mime, "video/x-ms-wmx");
                else if( strcmp(ctx->iformat->name, "rmvb") == 0 )
                        xasprintf(&m.mime, "application/vnd.rn-realmedia-vbr");
                else if( strcmp(ctx->iformat->name, ".wm") == 0 )
                        xasprintf(&m.mime, "video/x-ms-wm");
                else if( strcmp(ctx->iformat->name, ".mpeg") == 0 )
                        xasprintf(&m.mime, "video/mpg");
                else if( strcmp(ctx->iformat->name, ".mpeg2") == 0 )
                        xasprintf(&m.mime, "video/mpg");
                else if( strcmp(ctx->iformat->name, ".mpga") == 0 )
                        xasprintf(&m.mime, "audio/rn-mpeg");
                else if( strcmp(ctx->iformat->name, ".qt") == 0 )
                        xasprintf(&m.mime, "video/quicktime");
                else if( strcmp(ctx->iformat->name, ".wmz") == 0 )
                        xasprintf(&m.mime, "application/x-ms-wmz");
                else if( strcmp(ctx->iformat->name, ".wmd") == 0 )
                        xasprintf(&m.mime, "application/x-ms-wmd");
                else if( strcmp(ctx->iformat->name, ".f4v") == 0 )
                        xasprintf(&m.mime, "video/mp4");
                else if( strcmp(ctx->iformat->name, ".ts") == 0 )
                        xasprintf(&m.mime, "application/x-troll-ts");
                else if( strcmp(ctx->iformat->name, ".wvx") == 0 )
                        xasprintf(&m.mime, "video/x-ms-wvx");
#endif
		else
			DPRINTF(E_WARN, L_METADATA, "%s: Unhandled format: %s\n", path, ctx->iformat->name);
	}
	lav_close(ctx);

	if( !m.date )
	{
		m.date = malloc(20);
		modtime = localtime(&file.st_mtime);
		strftime(m.date, 20, "%FT%T", modtime);
	}

	if( !m.title )
		m.title = strdup(name);

	album_art = find_album_art(path, video.image, video.image_size);
	freetags(&video);
#ifdef BAIDU_DMS_OPT
	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE, TIMESTAMP, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
	                   "  TITLE, CREATOR, ARTIST, GENRE, COMMENT, DLNA_PN, MIME, ALBUM_ART) "
	                   "VALUES"
	                   " (%Q, %lld, %ld, %Q, %Q, %Q, %Q, %Q, %Q, '%q', %Q, %Q, %Q, %Q, %Q, '%q', %lld);",
	                   path, (long long)file.st_size, file.st_mtime, m.duration,
	                   m.date, m.channels, m.bitrate, m.frequency, m.resolution,
			   full_name, m.creator, m.artist, m.genre, m.comment, m.dlna_pn,
                           m.mime, album_art);
#else
	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE, TIMESTAMP, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
	                   "  TITLE, CREATOR, ARTIST, GENRE, COMMENT, DLNA_PN, MIME, ALBUM_ART) "
	                   "VALUES"
	                   " (%Q, %lld, %ld, %Q, %Q, %Q, %Q, %Q, %Q, '%q', %Q, %Q, %Q, %Q, %Q, '%q', %lld);",
	                   path, (long long)file.st_size, file.st_mtime, m.duration,
	                   m.date, m.channels, m.bitrate, m.frequency, m.resolution,
			   name, m.creator, m.artist, m.genre, m.comment, m.dlna_pn,
                           m.mime, album_art);
#endif
	if( ret != SQLITE_OK )
	{
		fprintf(stderr, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
		check_for_captions(path, ret);
	}
	free_metadata(&m, free_flags);
	free(path_cpy);

	return ret;
#else
	char full_name[64];
	snprintf(full_name,sizeof(full_name),"%s",name);
	char type[4];
	struct stat file;
	int ret, i;
	struct tm *modtime;
	int audio_stream = -1, video_stream = -1;
	enum audio_profiles audio_profile = PROFILE_AUDIO_UNKNOWN;
	char fourcc[4];
	int64_t album_art = 0;
	char nfo[MAXPATHLEN], *ext;
	struct song_metadata video;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	char *path_cpy, *basepath;
	memset(&m, '\0', sizeof(m));
	memset(&video, '\0', sizeof(video));

	//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Parsing video %s...\n", name);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);
	if( ends_with(path, ".avi") )
	{
		strcpy(type, "avi");
		m.mime = strdup("video/x-msvideo");
	}
    else if(ends_with(path, ".swf") )
	 {
		m.mime = strdup("application/x-shockwave-flash");

	  }
	else if( ends_with(path, ".mpeg") )
	{
		strcpy(type, "mpeg");
		m.mime = strdup("video/mpeg");
	}
	else if( ends_with(path, ".asf") )
	{
		strcpy(type, "asf");
		m.mime = strdup("video/x-ms-wmv");
	}

	else if(  ends_with(path, ".mp4") ||
		         ends_with(path, ".m4a") || ends_with(path, ".3gp") ||
		         ends_with(path, ".3g2") || ends_with(path, ".mj2"))
	{
		m.mime = strdup("video/mp4");
	}
	else if(ends_with(path, ".mov"))
	{
		m.mime = strdup("video/quicktime");

	}
	else if(ends_with(path, ".matroska"))
	{
		m.mime = strdup("video/x-matroska");

	}
	else if(ends_with(path, ".flv"))
	{
		m.mime = strdup("video/x-flv");

	}
	else if(ends_with(path, ".rm") )
	{
		m.mime = strdup("video/x-pn-realvideo");
	}
    else if(ends_with(path, ".mpeg4") )
    {
        m.mime = strdup("video/mpeg4");
    }
    else if( ends_with(path, ".wmx") )
    {
        m.mime = strdup("video/x-ms-wmx");
    }
    else if( ends_with(path, ".rmvb") )
    {
        m.mime = strdup("application/vnd.rn-realmedia-vbr");
    }
    else if( ends_with(path, ".wm") )
    {
        m.mime = strdup("video/x-ms-wm");
    }
    else if( ends_with(path, ".mpeg2") )
    {
        m.mime = strdup("video/mpg");
    }
    else if( ends_with(path, ".mpga") )
    {
        m.mime = strdup("audio/rn-mpeg");
    }
    else if( ends_with(path, ".qt") )
    {
        m.mime = strdup("video/quicktime");
    }
    else if( ends_with(path, ".wmz") )
    {
        m.mime = strdup("application/x-ms-wmz");
    }
    else if( ends_with(path, ".wmd") )
    {
        m.mime = strdup("application/x-ms-wmd");
    }
    else if( ends_with(path, ".f4v") )
    {
        m.mime = strdup("video/mp4");
    }
    else if( ends_with(path, ".ts") )
    {
        m.mime = strdup("application/x-troll-ts");
    }
    else if( ends_with(path, ".wvx") )
    {
        m.mime = strdup("video/x-ms-wvx");
    }
    else if( ends_with(path, ".mkv") )
    {
        m.mime = strdup("video/x-ms-wvx");
    }
    else if( ends_with(path, ".mpg") )
    {
        m.mime = strdup("video/mpeg");
    }
    else if( ends_with(path, ".m4v") )
    {
        m.mime = strdup("video/x-m4v");
    }
    else if( ends_with(path, ".wmv") )
    {
        m.mime = strdup("audio/x-ms-wmv");
    }
	else
	{
		DPRINTF(E_WARN, L_GENERAL, "Unhandled file extension on %s\n", path);
		return 0;
	}
	path_cpy = strdup(path);
	basepath = basename(path_cpy);

video_no_dlna:
	strcpy(nfo, path);
	ext = strrchr(nfo, '.');
	if( ext )
	{
		strcpy(ext+1, "nfo");
		if( access(nfo, F_OK) == 0 )
		{
			parse_nfo(nfo, &m);
		}
	}
	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, SIZE, TYPE,TIMESTAMP_mtime,TIMESTAMP_ctime, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
	                   "  TITLE, CREATOR, ARTIST, GENRE, COMMENT, DLNA_PN, MIME, ALBUM_ART) "
	                   "VALUES"
	                   " (%Q, %lld,%Q,%ld, %ld, %Q, %Q, %Q, %Q, %Q, %Q, '%q', %Q, %Q, %Q, %Q, %Q, '%q', %lld);",
	                   path, (long long)file.st_size,"video" ,file.st_mtime,file.st_ctime,NULL,
	                   NULL,  NULL, NULL, NULL, NULL,
			   full_name, NULL, NULL, NULL, NULL, NULL,
                           m.mime, album_art);
	if( ret != SQLITE_OK )
	{
		fprintf(stderr, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
		check_for_captions(path, ret);
	}
	free_metadata(&m, free_flags);
	free(path_cpy);

	return ret;
#endif
}

#ifdef NAS
int64_t
GetTextMetadata(const char *path, char *name)
{
	int dir_count=0,i=0;
	char full_dir[64];
	snprintf(full_dir,sizeof(full_dir),"%s",path);
	printf("full_dir:%s\n",full_dir);
	while(full_dir[i] != '\0')
	{
		if(dir_count > 11){

			return 0;
		}

		if('/'== full_dir[i]){
			dir_count++;
		}
		i++;
	}
	struct stat file;
	int64_t	    ret;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	memset(&m, '\0', sizeof(metadata_t));

	if ( stat(path, &file) != 0 )
		return 0;
	//strip_ext(name);
	if( ends_with(path, ".txt") )
	{
		m.mime = strdup("text/plain");
	}
	else if(ends_with(path, ".pdf") )
	{
		m.mime = strdup("application/pdf");
	}
	else if( ends_with(path, ".umd") )
	{
		m.mime = strdup("application/umd");
	}
	else if( ends_with(path, ".epub") )
	{
		m.mime = strdup("application/epub+zip");
	}
	else if(ends_with(path, ".doc") )
	{
		m.mime = strdup("application/msword");
	}
	else if( ends_with(path, ".xls") )
	{
		m.mime = strdup("application/vnd.ms-excel");
	}
	else if( ends_with(path, ".ppt") )
	{
		m.mime = strdup("application/vnd.ms-powerpoint");
	}
	else if(ends_with(path, ".docx") )
	{
		m.mime = strdup("application/vnd.openxmlformats-officedocument.wordprocessingml.document");
	}
	else if( ends_with(path, ".xlsx") )
	{
		m.mime = strdup("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
	}
	else if( ends_with(path, ".pptx") )
	{
		m.mime = strdup("application/vnd.openxmlformats-officedocument.presentationml.presentation");
	}
	else if( ends_with(path, ".chm") )
	{
		m.mime = strdup("application/mshelp");
	}
	else if(ends_with(path, ".html") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".dot") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".dotx") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".odm") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".ots") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".ods") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".odt") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".rtf") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".xlt") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".xltx") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".csv") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".pps") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".ppsx") )
	{
		m.mime = strdup("text/html");
	}
	else if(ends_with(path, ".potx") )
	{
		m.mime = strdup("text/html");
	}
	else
	{
		DPRINTF(E_WARN, L_GENERAL, "Unhandled file extension on %s\n", path);
		return 0;
	}
	ret = sql_exec(db2, "INSERT into Nas"
	                   " (PATH, TITLE, SIZE,TYPE, TIMESTAMP_mtime, TIMESTAMP_ctime,MIME) "
	                   "VALUES"
	                   " (%Q, '%q', %lld,%Q, %ld,%ld, %Q);",
	                   path, name, (long long)file.st_size,"text", file.st_mtime, file.st_ctime,m.mime);
	if( ret != SQLITE_OK )
	{
		fprintf(stderr, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db2);
	}
	free_metadata(&m, free_flags);

	return ret;
}
int64_t
GetAppMetadata(const char *path, char *name)
{
	struct stat file;
	int64_t	  ret;
	metadata_t m;
	uint32_t free_flags = 0xFFFFFFFF;
	memset(&m, '\0', sizeof(metadata_t));
	int dir_count=0,num=0;
	char full_dir[64];
	snprintf(full_dir,sizeof(full_dir),"%s",path);
	printf("full_dir:%s\n",full_dir);
	while(full_dir[num] != '\0')
	{
		if(dir_count > 11){

			return 0;
		}

		if('/'== full_dir[num]){
			dir_count++;
		}
		num++;
	}

	if ( stat(path, &file) != 0 )
		return 0;
	//strip_ext(name);

	if( ends_with(path, ".apk") )
	{
		m.mime = strdup("application/vnd.android.package-archive");
	}
	else if(ends_with(path, ".exe") )
	{
		m.mime = strdup("application/octet-stream");
	}
	else if( ends_with(path, ".msi") )
	{
		m.mime = strdup("application/octet-stream");
	}
	else if( ends_with(path, ".deb") )
	{
		m.mime = strdup("application/x-debian-package-archive");
	}
	else if( ends_with(path, ".ipa") )
	{
		m.mime = strdup("application/iphone-package-archive");
	}
	else if( ends_with(path, ".px") )
	{
		m.mime = strdup("application/x-ipix");
	}
	else
	{
		DPRINTF(E_WARN, L_GENERAL, "Unhandled file extension on %s\n", path);
		return 0;
	}
	ret = sql_exec(db2, "INSERT into Nas"
	                   " (PATH, TITLE, SIZE,TYPE, TIMESTAMP_mtime,TIMESTAMP_ctime, MIME) "
	                   "VALUES"
	                   " (%Q, '%q', %lld, %Q,%ld, %ld,%Q);",
	                   path, name, (long long)file.st_size,"app", file.st_mtime,file.st_ctime,m.mime);
	if( ret != SQLITE_OK )
	{
		fprintf(stderr, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db2);
	}
	free_metadata(&m, free_flags);

	return ret;
}

int64_t
GetAllFile(const char *path, const char *name, OPTION option, NAS_DIR dir)
{
	struct stat file;
	int64_t	  ret;
	int dir_count=0,num=0;
	char file_type[16];
	char full_dir[64];
	snprintf(full_dir,sizeof(full_dir),"%s",path);
	printf("full_dir:%s,%s,%d\n",full_dir,name,nas_timestamp);
	if((time(NULL) - share->flag_daemon) < 15){
		if(NULL == strstr(path, share->nas_share_path)){
			return 0;
		}
	}
	if(strrchr(name, '~'))
	{
		return 0;
	}
	while(full_dir[num] != '\0')
	{
		if(dir_count > 11){

			return 0;
		}

		if('/'== full_dir[num]){
			dir_count++;
		}
		num++;
	}
	nas_timestamp++;
	//mime = getmime(name);
	if(is_video(name))
	{
		snprintf(file_type,sizeof(file_type),"%s","vedio");
	}
	else if(is_audio(name))
	{
		snprintf(file_type,sizeof(file_type),"%s","audio");
	}
	else if(is_image(name))
	{
		snprintf(file_type,sizeof(file_type),"%s","image");
	}
	else if(is_text(name))
	{
		snprintf(file_type,sizeof(file_type),"%s","text");
	}
	else if(is_application(name))
	{
		snprintf(file_type,sizeof(file_type),"%s","app");
	}
	else
	{
		snprintf(file_type,sizeof(file_type),"%s","other");
	}
	if(0 == dir)
	{
		snprintf(file_type,sizeof(file_type),"%s","fold");
	}
	GetDiskInfo(share->nas_share_path);
	switch (option)
	{
	case add:
		if ( stat(path, &file) != 0 )
		{
			//free(mime);
			return 0;
		}
		nas_inotify_update_file(path , name, dir);
		ret = sql_exec(add_db, "INSERT into Nasadd"
				" (PATH, TITLE, SIZE,TYPE, TIMESTAMP_ctime, TIMESTAMP_mtime,TIMESTAMP, OPTION) "
				"VALUES"
				" (%Q, '%q', %lld, %Q, %ld, %ld, %d , %d);",
				path, name, (long long)file.st_size,file_type, file.st_ctime, file.st_mtime,nas_timestamp,option);
				break;
	case rm:
		/*
		if ( 0 == stat(path, &file) )
		{
			//free(mime);
			return 0;
		}
		*/
		ret = sql_exec(add_db, "INSERT into Nasrm"
				" (PATH, TITLE, SIZE,TYPE, TIMESTAMP_ctime, TIMESTAMP_mtime, TIMESTAMP, OPTION) "
				"VALUES"
				" (%Q, '%q', %d, %Q, %d, %d, %d, %d);",
				path, name, 0, file_type, 0, 0, nas_timestamp, option );
		break;
	case change:
		if ( stat(path, &file) != 0 )
		{

			return 0;
		}


		ret = sql_exec(add_db, "INSERT into Nasupdate"
				" (PATH, TITLE, SIZE,TYPE, TIMESTAMP_ctime, TIMESTAMP_mtime,TIMESTAMP,OPTION) "
				"VALUES"
				" (%Q, '%q', %lld, %Q,%ld, %ld,%d, %d);",
				path, name, (long long)file.st_size,file_type, file.st_ctime, file.st_mtime, nas_timestamp, option);
		break;
	default :
		DPRINTF(E_WARN, L_GENERAL, "reset option state \n");
		break;
	}

	if( ret != SQLITE_OK )
	{
		fprintf(stderr, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}

	return ret;
}
void
GetDiskInfo(char *path)
{
	struct stat file;
	if ( stat(path, &file) != 0 )
		return;
		sql_exec(add_db, "INSERT into Nasdiskinfo"
				" (PATH, SIZE, NASdir_mtime, NASdir_ctime) "
				"VALUES"
				" (%Q, %d, %ld, %ld );",
				path, 0, file.st_mtime, nas_timestamp);

	return;
}
#endif
