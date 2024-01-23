/*********************************************************************
 *   Copyright 2018, UCAR/Unidata
 *   See netcdf/COPYRIGHT file for copying and redistribution conditions.
 *********************************************************************/

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef _MSC_VER
#include <io.h>
#endif

#include "netcdf.h"
#include "ncuri.h"
#include "nclist.h"
#include "ncrc.h"
#include "ncs3sdk.h"

#undef AWSDEBUG

#define AWSHOST ".amazonaws.com"
#define GOOGLEHOST "storage.googleapis.com"

enum URLFORMAT {UF_NONE=0, UF_VIRTUAL=1, UF_PATH=2, UF_S3=3, UF_OTHER=4};

/* Forward */
static int endswith(const char* s, const char* suffix);

/**************************************************/
/* Generic S3 Utilities */

/*
Rebuild an S3 url into a canonical path-style url.
If region is not in the host, then use specified region
if provided, otherwise us-east-1.
@param url (in) the current url
@param s3	(in/out) NCS3INFO structure
@param pathurlp (out) the resulting pathified url string
*/

int
NC_s3urlrebuild(NCURI* url, NCS3INFO* s3, NCURI** newurlp)
{
    size_t i;
    int stat = NC_NOERR;
    NClist* hostsegments = NULL;
    NClist* pathsegments = NULL;
    NCbytes* buf = ncbytesnew();
    NCURI* newurl = NULL;
    char* bucket = NULL;
    char* host = NULL;
    char* path = NULL;
    char* region = NULL;
    NCS3SVC svc = NCS3UNK;
    
    if(url == NULL)
        {stat = NC_EURL; goto done;}

    /* Parse the hostname */
    hostsegments = nclistnew();
    /* split the hostname by "." */
    if((stat = NC_split_delim(url->host,'.',hostsegments))) goto done;

    /* Parse the path*/
    pathsegments = nclistnew();
    /* split the path by "/" */
    if((stat = NC_split_delim(url->path,'/',pathsegments))) goto done;

    /* Distinguish path-style from virtual-host style from s3: and from other.
       Virtual: https://<bucket-name>.s3.<region>.amazonaws.com/<path>				(1)
            or: https://<bucket-name>.s3.amazonaws.com/<path> -- region defaults to us-east-1	(2)
       Path: https://s3.<region>.amazonaws.com/<bucket-name>/<path>				(3)
         or: https://s3.amazonaws.com/<bucket-name>/<path> -- region defaults to us-east-1      (4)
       S3: s3://<bucket-name>/<path>								(5)
       Google: https://storage.googleapis.com/<bucket-name>/<path>				(6)
           or: gs3://<bucket-name>/<path>							(7)
       Other: https://<host>/<bucket-name>/<path>						(8)
    */
    if(url->host == NULL || strlen(url->host) == 0)
        {stat = NC_EURL; goto done;}

    /* Reduce the host to standard form such as s3.amazonaws.com by pulling out the
       region and bucket from the host */
    if(strcmp(url->protocol,"s3")==0 && nclistlength(hostsegments)==1) { /* Format (5) */
	bucket = nclistremove(hostsegments,0);
	/* region unknown at this point */
	/* Host will be set to canonical form later */
	svc = NCS3;
    } else if(strcmp(url->protocol,"gs3")==0 && nclistlength(hostsegments)==1) { /* Format (7) */
	bucket = nclistremove(hostsegments,0);
	/* region unknown at this point */
	/* Host will be set to canonical form later */
	svc = NCS3GS;
    } else if(endswith(url->host,AWSHOST)) { /* Virtual or path */
	svc = NCS3;
	/* If we find a bucket as part of the host, then remove it */
	switch (nclistlength(hostsegments)) {
	default: stat = NC_EURL; goto done;
	case 3: /* Format (4) */ 
	    /* region unknown at this point */
    	    /* bucket unknown at this point */
	    break;
	case 4: /* Format (2) or (3) */
            if(strcasecmp(nclistget(hostsegments,0),"s3")!=0) { /* Presume format (2) */
	        /* region unknown at this point */
	        bucket = nclistremove(hostsegments,0); /* Make canonical */
            } else if(strcasecmp(nclistget(hostsegments,0),"s3")==0) { /* Format (3) */
	        region = nclistremove(hostsegments,1); /* Make canonical */
	        /* bucket unknown at this point */
	    } else /* ! Format (2) and ! Format (3) => error */
	        {stat = NC_EURL; goto done;}
	    break;
	case 5: /* Format (1) */
            if(strcasecmp(nclistget(hostsegments,1),"s3")!=0)
	        {stat = NC_EURL; goto done;}
	    /* Make canonical */
	    region = nclistremove(hostsegments,2);
    	    bucket = nclistremove(hostsegments,0);
	    break;
	}
    } else if(strcasecmp(url->host,GOOGLEHOST)==0) { /* Google (6) */
        if((host = strdup(url->host))==NULL)
	    {stat = NC_ENOMEM; goto done;}
        /* region is unknown */
	/* bucket is unknown at this point */
	svc = NCS3GS;
    } else { /* Presume Format (8) */
        if((host = strdup(url->host))==NULL)
	    {stat = NC_ENOMEM; goto done;}
        /* region is unknown */
	/* bucket is unknown */
    }

    /* region = (1) from url, (2) s3->region, (3) default */
    if(region == NULL && s3 != NULL)
	region = nulldup(s3->region);
    if(region == NULL) {
        const char* region0 = NULL;
	/* Get default region */
	if((stat = NC_getdefaults3region(url,&region0))) goto done;
	region = strdup(region0);
    }
    if(region == NULL) {stat = NC_ES3; goto done;}

    /* bucket = (1) from url, (2) s3->bucket */
    if(bucket == NULL && nclistlength(pathsegments) > 0) {
	bucket = nclistremove(pathsegments,0); /* Get from the URL path; will reinsert below */
    }
    if(bucket == NULL && s3 != NULL)
	bucket = nulldup(s3->bucket);
    if(bucket == NULL) {stat = NC_ES3; goto done;}

    if(svc == NCS3) {
        /* Construct the revised host */
	ncbytesclear(buf);
        ncbytescat(buf,"s3.");
        ncbytescat(buf,region);
        ncbytescat(buf,AWSHOST);
	nullfree(host);
        host = ncbytesextract(buf);
    } else if(svc == NCS3GS) {
	nullfree(host);
	host = strdup(GOOGLEHOST);
    }

    ncbytesclear(buf);

    /* Construct the revised path */
    if(bucket != NULL) {
        ncbytescat(buf,"/");
        ncbytescat(buf,bucket);
    }
    for(i=0;i<nclistlength(pathsegments);i++) {
	ncbytescat(buf,"/");
	ncbytescat(buf,nclistget(pathsegments,i));
    }
    path = ncbytesextract(buf);

    /* complete the new url */
    if((newurl=ncuriclone(url))==NULL) {stat = NC_ENOMEM; goto done;}
    ncurisetprotocol(newurl,"https");
    assert(host != NULL);
    ncurisethost(newurl,host);
    assert(path != NULL);
    ncurisetpath(newurl,path);
    /* Rebuild the url->url */
    ncurirebuild(newurl);
    /* return various items */
#ifdef AWSDEBUG
    fprintf(stderr,">>> NC_s3urlrebuild: final=%s bucket=%s region=%s\n",uri->uri,bucket,region);
#endif
    if(newurlp) {*newurlp = newurl; newurl = NULL;}
    if(s3 != NULL) {
        s3->bucket = bucket; bucket = NULL;
        s3->region = region; region = NULL;
        s3->svc = svc;
    }
done:
    nullfree(region);
    nullfree(bucket)
    nullfree(host)
    nullfree(path)
    ncurifree(newurl);
    ncbytesfree(buf);
    nclistfreeall(hostsegments);
    nclistfreeall(pathsegments);
    return stat;
}

static int
endswith(const char* s, const char* suffix)
{
    ssize_t ls, lsf, delta;
    if(s == NULL || suffix == NULL) return 0;
    ls = strlen(s);
    lsf = strlen(suffix);
    delta = (ls - lsf);
    if(delta < 0) return 0;
    if(memcmp(s+delta,suffix,lsf)!=0) return 0;
    return 1;
}

/**************************************************/
/* S3 utilities */

EXTERNL int
NC_s3urlprocess(NCURI* url, NCS3INFO* s3, NCURI** newurlp)
{
    int stat = NC_NOERR;
    NCURI* url2 = NULL;
    NClist* pathsegments = NULL;
    const char* profile0 = NULL;

    if(url == NULL || s3 == NULL)
        {stat = NC_EURL; goto done;}
    /* Get current profile */
    if((stat = NC_getactives3profile(url,&profile0))) goto done;
    if(profile0 == NULL) profile0 = "no";
    s3->profile = strdup(profile0);

    /* Rebuild the URL to path format and get a usable region and optional bucket*/
    if((stat = NC_s3urlrebuild(url,s3,&url2))) goto done;
    s3->host = strdup(url2->host);
    /* construct the rootkey minus the leading bucket */
    pathsegments = nclistnew();
    if((stat = NC_split_delim(url2->path,'/',pathsegments))) goto done;
    if(nclistlength(pathsegments) > 0) {
	char* seg = nclistremove(pathsegments,0);
        nullfree(seg);
    }
    if((stat = NC_join(pathsegments,&s3->rootkey))) goto done;
    if(newurlp) {*newurlp = url2; url2 = NULL;}

done:
    ncurifree(url2);
    nclistfreeall(pathsegments);
    return stat;
}

int
NC_s3clone(NCS3INFO* s3, NCS3INFO** news3p)
{
    NCS3INFO* news3 = NULL;
    if(s3 && news3p) {
	if((news3 = (NCS3INFO*)calloc(1,sizeof(NCS3INFO)))==NULL)
           return NC_ENOMEM;
	if((news3->host = nulldup(s3->host))==NULL) return NC_ENOMEM;
	if((news3->region = nulldup(s3->region))==NULL) return NC_ENOMEM;
	if((news3->bucket = nulldup(s3->bucket))==NULL) return NC_ENOMEM;
	if((news3->rootkey = nulldup(s3->rootkey))==NULL) return NC_ENOMEM;
	if((news3->profile = nulldup(s3->profile))==NULL) return NC_ENOMEM;
    }
    if(news3p) {*news3p = news3; news3 = NULL;}
    else {NC_s3clear(news3); nullfree(news3);}
    return NC_NOERR;
}

int
NC_s3clear(NCS3INFO* s3)
{
    if(s3) {
	nullfree(s3->host); s3->host = NULL;
	nullfree(s3->region); s3->region = NULL;
	nullfree(s3->bucket); s3->bucket = NULL;
	nullfree(s3->rootkey); s3->rootkey = NULL;
	nullfree(s3->profile); s3->profile = NULL;
    }
    return NC_NOERR;
}

/*
Check if a url has indicators that signal an S3 or Google S3 url.
*/

int
NC_iss3(NCURI* uri)
{
    int iss3 = 0;

    if(uri == NULL) goto done; /* not a uri */
    /* is the protocol "s3" or "gs3" ? */
    if(strcasecmp(uri->protocol,"s3")==0) {iss3 = 1; goto done;}
    if(strcasecmp(uri->protocol,"gs3")==0) {iss3 = 1; goto done;}
    /* Is "s3" or "gs3" in the mode list? */
    if(NC_testmode(uri,"s3")) {iss3 = 1; goto done;}
    if(NC_testmode(uri,"gs3")) {iss3 = 1; goto done;}    
    /* Last chance; see if host looks s3'y */
    if(uri->host != NULL) {
        if(endswith(uri->host,AWSHOST)) {iss3 = 1; goto done;}
        if(strcasecmp(uri->host,GOOGLEHOST)==0) {iss3 = 1; goto done;}
    }    
done:
    return iss3;
}

const char*
NC_s3dumps3info(NCS3INFO* info)
{
    static char text[8192];
    snprintf(text,sizeof(text),"host=%s region=%s bucket=%s rootkey=%s profile=%s",
		(info->host?info->host:"null"),
		(info->region?info->region:"null"),
		(info->bucket?info->bucket:"null"),
		(info->rootkey?info->rootkey:"null"),
		(info->profile?info->profile:"null"));
    return text;
}

