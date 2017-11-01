/* Copyright 2009, UCAR/Unidata and OPeNDAP, Inc.
   See the COPYRIGHT file for more information. */

#include "config.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef _WIN32
#include <io.h>
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif
#endif
#include "ocinternal.h"
#include "ocdebug.h"
#include "ochttp.h"
#include "ocread.h"
#include "occurlfunctions.h"
#include "ncwinpath.h"

/*Forward*/
static int readpacket(OCstate* state, NCURI*, NCbytes*, OCdxd, long*);
static int readfile(const char* path, const char* suffix, NCbytes* packet);
static int readfiletofile(const char* path, const char* suffix, FILE* stream, off_t*);

int
readDDS(OCstate* state, OCtree* tree)
{
    int stat = OC_NOERR;
    long lastmodified = -1;

    ncurisetquery(state->uri,tree->constraint);

#ifdef OCDEBUG
fprintf(stderr,"readDDS:\n");
#endif
    stat = readpacket(state,state->uri,state->packet,OCDDS,
			&lastmodified);
    if(stat == OC_NOERR) state->ddslastmodified = lastmodified;

    return stat;
}

int
readDAS(OCstate* state, OCtree* tree)
{
    int stat = OC_NOERR;

    ncurisetquery(state->uri,tree->constraint);
#ifdef OCDEBUG
fprintf(stderr,"readDAS:\n");
#endif
    stat = readpacket(state,state->uri,state->packet,OCDAS,NULL);

    return stat;
}

#if 0
int
readversion(OCstate* state, NCURI* url, NCbytes* packet)
{
   return readpacket(state,url,packet,OCVER,NULL);
}
#endif

const char*
ocdxdextension(OCdxd dxd)
{
    switch(dxd) {
    case OCDDS: return ".dds";
    case OCDAS: return ".das";
    case OCDATADDS: return ".dods";
    default: break;
    }
    return NULL;
}

static int
readpacket(OCstate* state, NCURI* url,NCbytes* packet,OCdxd dxd,long* lastmodified)
{
   int stat = OC_NOERR;
   int fileprotocol = 0;
   const char* suffix = ocdxdextension(dxd);
   char* fetchurl = NULL;
   CURL* curl = state->curl;

   fileprotocol = (strcmp(url->protocol,"file")==0);

   if(fileprotocol && !state->curlflags.proto_file) {
        /* Short circuit file://... urls*/
	/* We do this because the test code always needs to read files*/
	fetchurl = ncuribuild(url,NULL,NULL,NCURIBASE);
	stat = readfile(fetchurl,suffix,packet);
    } else {
	int flags = NCURIBASE;
	if(!fileprotocol) flags |= NCURIQUERY;
	flags |= NCURIENCODE;
        fetchurl = ncuribuild(url,NULL,suffix,flags);
	MEMCHECK(fetchurl,OC_ENOMEM);
	if(ocdebug > 0)
            {fprintf(stderr,"fetch url=%s\n",fetchurl); fflush(stderr);}
        stat = ocfetchurl(curl,fetchurl,packet,lastmodified,&state->creds);
	if(stat)
	    oc_curl_printerror(state);
	if(ocdebug > 0)
            {fprintf(stderr,"fetch complete\n"); fflush(stderr);}
    }
    free(fetchurl);
#ifdef OCDEBUG
  {
fprintf(stderr,"readpacket: packet.size=%lu\n",
		(unsigned long)ncbyteslength(packet));
  }
#endif
    return OCTHROW(stat);
}

int
readDATADDS(OCstate* state, OCtree* tree, OCflags flags)
{
    int stat = OC_NOERR;
    long lastmod = -1;

#ifdef OCDEBUG
fprintf(stderr,"readDATADDS:\n");
#endif
    if((flags & OCONDISK) == 0) {
        ncurisetquery(state->uri,tree->constraint);
        stat = readpacket(state,state->uri,state->packet,OCDATADDS,&lastmod);
        if(stat == OC_NOERR)
            state->datalastmodified = lastmod;
        tree->data.datasize = ncbyteslength(state->packet);
    } else { /*((flags & OCONDISK) != 0) */
        NCURI* url = state->uri;
        int fileprotocol = 0;
        char* readurl = NULL;

        fileprotocol = (strcmp(url->protocol,"file")==0);

        if(fileprotocol && !state->curlflags.proto_file) {
            readurl = ncuribuild(url,NULL,NULL,NCURIBASE);
            stat = readfiletofile(readurl, ".dods", tree->data.file, &tree->data.datasize);
        } else {
            int flags = NCURIBASE;
            if(!fileprotocol) flags |= NCURIQUERY;
            flags |= NCURIENCODE;
            ncurisetquery(url,tree->constraint);
            readurl = ncuribuild(url,NULL,".dods",flags);
            MEMCHECK(readurl,OC_ENOMEM);
            if (ocdebug > 0) 
                {fprintf(stderr, "fetch url=%s\n", readurl);fflush(stderr);}
            stat = ocfetchurl_file(state->curl, readurl, tree->data.file,
                                   &tree->data.datasize, &lastmod);
            if(stat == OC_NOERR)
                state->datalastmodified = lastmod;
            if (ocdebug > 0) 
                {fprintf(stderr,"fetch complete\n"); fflush(stderr);}
        }
        free(readurl);
    }
    return OCTHROW(stat);
}

static int
readfiletofile(const char* path, const char* suffix, FILE* stream, off_t* sizep)
{
    int stat = OC_NOERR;
    NCbytes* packet = ncbytesnew();
    size_t len;
    /* check for leading file:/// */
    if(ocstrncmp(path,"file:///",8)==0) path += 7; /* assume absolute path*/
    stat = readfile(path,suffix,packet);
#ifdef OCDEBUG
fprintf(stderr,"readfiletofile: packet.size=%lu\n",
		(unsigned long)ncbyteslength(packet));
#endif
    if(stat != OC_NOERR) goto unwind;
    len = nclistlength(packet);
    if(stat == OC_NOERR) {
	size_t written;
        fseek(stream,0,SEEK_SET);
	written = fwrite(ncbytescontents(packet),1,len,stream);
	if(written != len) {
#ifdef OCDEBUG
fprintf(stderr,"readfiletofile: written!=length: %lu :: %lu\n",
	(unsigned long)written,(unsigned long)len);
#endif
	    stat = OC_EIO;
	}
    }
    if(sizep != NULL) *sizep = len;
unwind:
    ncbytesfree(packet);
    return OCTHROW(stat);
}

static int
readfile(const char* path, const char* suffix, NCbytes* packet)
{
    int stat = OC_NOERR;
    char buf[1024];
    char filename[1024];
    int fd = -1;
    int flags = 0;
    off_t filesize = 0;
    off_t totalread = 0;
    /* check for leading file:/// */
    if(ocstrncmp(path,"file://",7)==0) path += 7; /* assume absolute path*/
    if(snprintf(filename,sizeof(filename),"%s%s",path,(suffix != NULL ? suffix : "")) >= sizeof(filename))
	return OCTHROW(OC_EOVERRUN);
    flags = O_RDONLY;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    fd = NCopen2(filename,flags);
    if(fd < 0) {
	nclog(NCLOGERR,"open failed: %s file=|%s|",ocerrstring(errno),filename);
	stat = OC_EOPEN;
	goto done;
    }
    /* Get the file size */
    filesize = lseek(fd,(off_t)0,SEEK_END);
    if(filesize < 0) {
	stat = OC_EIO;
	nclog(NCLOGERR,"lseek failed: %s",filename);
	goto done;
    }
    /* Move file pointer back to the beginning of the file */
    (void)lseek(fd,(off_t)0,SEEK_SET);
    stat = OC_NOERR;
    for(totalread=0;;) {
	off_t count = (off_t)read(fd,buf,sizeof(buf));
	if(count == 0)
	    break; /*eof*/
	else if(count <  0) {
	    stat = OC_EIO;
	    nclog(NCLOGERR,"read failed: %s",filename);
	    goto done;
	}
	ncbytesappendn(packet,buf,(unsigned long)count);
	totalread += count;
    }
    if(totalread < filesize) {
	stat = OC_EIO;
	nclog(NCLOGERR,"short read: |%s|=%lu read=%lu\n",
		filename,(unsigned long)filesize,(unsigned long)totalread);
        goto done;
    }

done:
#ifdef OCDEBUG
fprintf(stderr,"readfile: filesize=%lu totalread=%lu\n",
		(unsigned long)filesize,(unsigned long)totalread);
#endif
    if(fd >= 0) close(fd);
    return OCTHROW(stat);
}


