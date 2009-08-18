/*
 * $Id$
 *
 * Copyright (c) 2008-2009 Nominet UK. All rights reserved.
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
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/* 
 * daemon_util.c code needed to get a daemon up and running
 *
 * edit the DAEMONCONFIG and cmlParse function
 * in daemon_util.[c|h] to add options specific
 * to your app
 *
 * gcc -o daemon daemon_util.c daemon.c
 *
 * Most of this is based on stuff I have seen in NSD
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <stdarg.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/relaxng.h>

#include "config.h"
#include "daemon.h"
#include "daemon_util.h"

#include "ksm/database.h"
#include "ksm/datetime.h"
#include "ksm/string_util.h"
#include "ksm/string_util2.h"

    int
permsDrop(DAEMONCONFIG* config)
{
    int status = 0;

    xmlDocPtr doc = NULL;
    xmlDocPtr rngdoc = NULL;
    xmlXPathContextPtr xpathCtx = NULL;
    xmlXPathObjectPtr xpathObj = NULL;
    xmlRelaxNGParserCtxtPtr rngpctx = NULL;
    xmlRelaxNGValidCtxtPtr rngctx = NULL;
    xmlRelaxNGPtr schema = NULL;
    xmlChar *user_expr = (unsigned char*) "//Configuration/Enforcer/Privileges/User";
    xmlChar *group_expr = (unsigned char*) "//Configuration/Enforcer/Privileges/Group";

    char* filename = CONFIG_FILE;
    char* rngfilename = SCHEMA_DIR "/conf.rng";

    char* temp_char = NULL;
    struct passwd *pwd;
    struct group *grp;
    
    /* Load XML document */
    doc = xmlParseFile(filename);
    if (doc == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to parse file \"%s\"\n", filename);
        return(-1);
    }

    /* Load rng document */
    rngdoc = xmlParseFile(rngfilename);
    if (rngdoc == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to parse file \"%s\"\n", rngfilename);
        return(-1);
    }

    /* Create an XML RelaxNGs parser context for the relax-ng document. */
    rngpctx = xmlRelaxNGNewDocParserCtxt(rngdoc);
    if (rngpctx == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to create XML RelaxNGs parser context\n");
        return(-1);
    }

    /* parse a schema definition resource and build an internal XML Shema struture which can be used to validate instances. */
    schema = xmlRelaxNGParse(rngpctx);
    if (schema == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to parse a schema definition resource\n");
        return(-1);
    }

    /* Create an XML RelaxNGs validation context based on the given schema */
    rngctx = xmlRelaxNGNewValidCtxt(schema);
    if (rngctx == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to create RelaxNGs validation context based on the schema\n");
        return(-1);
    }

    /* Validate a document tree in memory. */
    status = xmlRelaxNGValidateDoc(rngctx,doc);
    if (status != 0) {
        log_msg(config, LOG_ERR, "Error validating file \"%s\"\n", filename);
        return(-1);
    }

    /* Now parse a value out of the conf */
    /* Create xpath evaluation context */
    xpathCtx = xmlXPathNewContext(doc);
    if(xpathCtx == NULL) {
        log_msg(config, LOG_ERR,"Error: unable to create new XPath context\n");
        xmlFreeDoc(doc);
        return(-1);
    }
   
    /* Set the group if specified; else just set the gid as the real one */
    xpathObj = xmlXPathEvalExpression(group_expr, xpathCtx);
    if(xpathObj == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", group_expr);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }
    if (xpathObj->nodesetval->nodeNr > 0) {
        temp_char = (char*) xmlXPathCastToString(xpathObj);
        StrAppend(&config->groupname, temp_char);
        StrFree(temp_char);
        xmlXPathFreeObject(xpathObj);

        /* Lookup the group id in /etc/groups */
        if ((grp = getgrnam(config->groupname)) == NULL) {
            log_msg(config, LOG_ERR, "group '%s' does not exist. exiting...", config->groupname);
            exit(1);
        } else {
            config->gid = grp->gr_gid;
        }
        endgrent();

        if (setgid(config->gid) != 0) {
            log_msg(config, LOG_ERR, "unable to drop group privileges: %s", strerror(errno));
            xmlXPathFreeContext(xpathCtx);
            xmlFreeDoc(doc);
            return -1;
        }
        log_msg(config, LOG_INFO, "group set to: %s(%d)\n", config->groupname, config->gid);
    } else {
        config->gid = getgid();
    }

    /* Set the user to drop to if specified; else just set the uid as the real one */
    xpathObj = xmlXPathEvalExpression(user_expr, xpathCtx);
    if(xpathObj == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", user_expr);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }
    if (xpathObj->nodesetval->nodeNr > 0) {
        temp_char = (char*) xmlXPathCastToString(xpathObj);
        StrAppend(&config->username, temp_char);
        StrFree(temp_char);
        xmlXPathFreeObject(xpathObj);

        /* Lookup the user id in /etc/passwd */
        if ((pwd = getpwnam(config->username)) == NULL) {
            log_msg(config, LOG_ERR, "user '%s' does not exist. exiting...", config->username);
            exit(1);
        } else {
            config->uid = pwd->pw_uid;
        }
        endpwent();

        if (setuid(config->uid) != 0) {
            log_msg(config, LOG_ERR, "unable to drop user privileges: %s", strerror(errno));
            xmlXPathFreeContext(xpathCtx);
            xmlFreeDoc(doc);
            return -1;
        }
        log_msg(config, LOG_INFO, "user set to: %s(%d)\n", config->username, config->uid);
    } else {
        config->uid = getuid();
    }

    xmlXPathFreeContext(xpathCtx);
    xmlRelaxNGFree(schema);
    xmlRelaxNGFreeValidCtxt(rngctx);
    xmlRelaxNGFreeParserCtxt(rngpctx);
    xmlFreeDoc(doc);
    xmlFreeDoc(rngdoc);

    return 0;
}

/* Set up logging as per default (facility may be switched based on config file) */
void log_init(int facility, const char *program_name)
{
	openlog(program_name, 0, facility);
}

/* Switch log to new facility */
void log_switch(int facility, const char *facility_name, const char *program_name)
{
    closelog();
	openlog(program_name, 0, facility);
    log_msg(NULL, LOG_INFO, "Switched log facility to: %s", facility_name);
}


    void
log_msg(DAEMONCONFIG *config, int priority, const char *format, ...)
{
    /* TODO: if the variable arg list is bad then random errors can occur */ 
    va_list args;
    if (config && config->debug) priority = LOG_ERR;
    va_start(args, format);
    vsyslog(priority, format, args);
    va_end(args);
}

/*
 * log function suitable for libksm callback
 */
    void
ksm_log_msg(const char *format)
{
    syslog(LOG_ERR, "%s", format);
}

    static void
usage(const char* prog)
{
    fprintf(stderr, "Usage: %s [OPTION]...\n", prog);
    fprintf(stderr, "OpenDNSSEC Enforcer Daemon.\n\n");
    fprintf(stderr, "Supported options:\n");
    fprintf(stderr, "  -d          Debug.\n");
/*    fprintf(stderr, "  -u user     Change effective uid to the specified user.\n");*/
    fprintf(stderr, "  -P pidfile  Specify the PID file to write.\n");

    fprintf(stderr, "  -v          Print version.\n");
    fprintf(stderr, "  -[?|h]      This help.\n");
}

    static void
version(void)
{
    fprintf(stderr, "%s version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
    fprintf(stderr, "Written by %s.\n\n", AUTHOR_NAME);
    fprintf(stderr, "%s.  This is free software.\n", COPYRIGHT_STR);
    fprintf(stderr, "See source files for more license information\n");
    exit(0);
}

    int
write_data(DAEMONCONFIG *config, FILE *file, const void *data, size_t size)
{
    size_t result;

    if (size == 0)
        return 1;

    result = fwrite(data, 1, size, file);

    if (result == 0) {
        log_msg(config, LOG_ERR, "write failed: %s", strerror(errno));
        return 0;
    } else if (result < size) {
        log_msg(config, LOG_ERR, "short write (disk full?)");
        return 0;
    } else {
        return 1;
    }
}

    int
writepid (DAEMONCONFIG *config)
{
    FILE * fd;
    char pidbuf[32];

    snprintf(pidbuf, sizeof(pidbuf), "%lu\n", (unsigned long) config->pid);

    if ((fd = fopen(config->pidfile, "w")) ==  NULL ) {
        return -1;
    }

    if (!write_data(config, fd, pidbuf, strlen(pidbuf))) {
        fclose(fd);
        return -1;
    }
    fclose(fd);

    if (chown(config->pidfile, config->uid, config->gid) == -1) {
        log_msg(config, LOG_ERR, "cannot chown %u.%u %s: %s",
                (unsigned) config->uid, (unsigned) config->gid,
                config->pidfile, strerror(errno));
        return -1;
    }

    return 0;
}



    void
cmdlParse(DAEMONCONFIG* config, int *argc, char **argv)
{
    int c;

    /*
     * Read the command line
     */
    while ((c = getopt(*argc, argv, "hdv?u:P:")) != -1) {
        switch (c) {
            case 'd':
                config->debug = true;
                break;
            case 'P':
                config->pidfile = optarg;
                break;
            case 'u':
                break; /* disable this feature */
                config->username = optarg;
                /* Parse the username into uid and gid */
                config->gid = getgid();
                config->uid = getuid();
                if (*config->username) {
                    struct passwd *pwd;
                    if (isdigit(*config->username)) {
                        char *t;
                        config->uid = strtol(config->username, &t, 10);
                        if (*t != 0) {
                            if (*t != '.' || !isdigit(*++t)) {
                                log_msg(config, LOG_ERR, "-u user or -u uid or -u uid.gid. exiting...");
                                exit(1);
                            }
                            config->gid = strtol(t, &t, 10);
                        } else {
                            /* Lookup the group id in /etc/passwd */
                            if ((pwd = getpwuid(config->uid)) == NULL) {
                                log_msg(config, LOG_ERR, "user id %u does not exist. exiting...", (unsigned) config->uid);
                                exit(1);
                            } else {
                                config->gid = pwd->pw_gid;
                            }
                            endpwent();
                        }
                    } else {
                        /* Lookup the user id in /etc/passwd */
                        if ((pwd = getpwnam(config->username)) == NULL) {
                            log_msg(config, LOG_ERR, "user '%s' does not exist. exiting...", config->username);
                            exit(1);
                        } else {
                            config->uid = pwd->pw_uid;
                            config->gid = pwd->pw_gid;
                        }
                        endpwent();
                    }
                }   
                break;
            case 'h':
                usage(config->program);
                exit(0);
            case '?':
                usage(config->program);
                exit(0);
            case 'v':
                version();
                exit(0);
            default:
                usage(config->program);
                exit(0);
        }
    }
}

int
ReadConfig(DAEMONCONFIG *config)
{
    xmlDocPtr doc = NULL;
    xmlDocPtr rngdoc = NULL;
    xmlXPathContextPtr xpathCtx = NULL;
    xmlXPathObjectPtr xpathObj = NULL;
    xmlRelaxNGParserCtxtPtr rngpctx = NULL;
    xmlRelaxNGValidCtxtPtr rngctx = NULL;
    xmlRelaxNGPtr schema = NULL;
    xmlChar *ki_expr = (unsigned char*) "//Configuration/Enforcer/KeygenInterval";
    xmlChar *iv_expr = (unsigned char*) "//Configuration/Enforcer/Interval";
    xmlChar *bi_expr = (unsigned char*) "//Configuration/Enforcer/BackupDelay";
    xmlChar *litexpr = (unsigned char*) "//Configuration/Enforcer/Datastore/SQLite";
    xmlChar *mysql_host = (unsigned char*) "//Configuration/Enforcer/Datastore/MySQL/Host";
    xmlChar *mysql_port = (unsigned char*) "//Configuration/Enforcer/Datastore/MySQL/Host/@port";
    xmlChar *mysql_db = (unsigned char*) "//Configuration/Enforcer/Datastore/MySQL/Database";
    xmlChar *mysql_user = (unsigned char*) "//Configuration/Enforcer/Datastore/MySQL/Username";
    xmlChar *mysql_pass = (unsigned char*) "//Configuration/Enforcer/Datastore/MySQL/Password";
    xmlChar *log_user_expr = (unsigned char*) "//Configuration/Common/Logging/Syslog/Facility";

    int mysec = 0;
    char *logFacilityName;
    int my_log_user = DEFAULT_LOG_FACILITY;
    int status;
    int db_found = 0;
    char* filename = CONFIG_FILE;
    char* rngfilename = SCHEMA_DIR "/conf.rng";

    char* temp_char = NULL;
    unsigned char* temp_uchar = NULL;

    log_msg(config, LOG_INFO, "Reading config \"%s\"\n", filename);

    /* Load XML document */
    doc = xmlParseFile(filename);
    if (doc == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to parse file \"%s\"\n", filename);
        return(-1);
    }

    /* Load rng document */
    log_msg(config, LOG_INFO, "Reading config schema \"%s\"\n", rngfilename);
    rngdoc = xmlParseFile(rngfilename);
    if (rngdoc == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to parse file \"%s\"\n", rngfilename);
        return(-1);
    }

    /* Create an XML RelaxNGs parser context for the relax-ng document. */
    rngpctx = xmlRelaxNGNewDocParserCtxt(rngdoc);
    if (rngpctx == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to create XML RelaxNGs parser context\n");
        return(-1);
    }

    /* parse a schema definition resource and build an internal XML Shema struture which can be used to validate instances. */
    schema = xmlRelaxNGParse(rngpctx);
    if (schema == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to parse a schema definition resource\n");
        return(-1);
    }

    /* Create an XML RelaxNGs validation context based on the given schema */
    rngctx = xmlRelaxNGNewValidCtxt(schema);
    if (rngctx == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to create RelaxNGs validation context based on the schema\n");
        return(-1);
    }

    /* Validate a document tree in memory. */
    status = xmlRelaxNGValidateDoc(rngctx,doc);
    if (status != 0) {
        log_msg(config, LOG_ERR, "Error validating file \"%s\"\n", filename);
        return(-1);
    }
    xmlRelaxNGFreeValidCtxt(rngctx);
    xmlRelaxNGFree(schema);
    xmlRelaxNGFreeParserCtxt(rngpctx);
    xmlFreeDoc(rngdoc);

    /* Now parse a value out of the conf */
    /* Create xpath evaluation context */
    xpathCtx = xmlXPathNewContext(doc);
    if(xpathCtx == NULL) {
        log_msg(config, LOG_ERR,"Error: unable to create new XPath context\n");
        xmlFreeDoc(doc);
        return(-1);
    }

    /* Evaluate xpath expression for keygen interval */
    xpathObj = xmlXPathEvalExpression(ki_expr, xpathCtx);
    if(xpathObj == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", ki_expr);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }

    temp_char = (char *)xmlXPathCastToString(xpathObj);
    status = DtXMLIntervalSeconds(temp_char, &mysec);
    if (status > 0) {
        log_msg(config, LOG_ERR, "Error: unable to convert interval %s to seconds, error: %i\n", temp_char, status);
        StrFree(temp_char);
        return status;
    }
    else if (status == -1) {
        log_msg(config, LOG_INFO, "Warning: converting %s to seconds may not give what you expect\n", temp_char);
    }
    config->keygeninterval = mysec;
    log_msg(config, LOG_INFO, "Key Generation Interval: %i\n", config->keygeninterval);
    StrFree(temp_char);
    xmlXPathFreeObject(xpathObj);

    /* Evaluate xpath expression for interval */
    /* TODO check that we can reuse xpathObj even if something has not worked */
    xpathObj = xmlXPathEvalExpression(iv_expr, xpathCtx);
    if(xpathObj == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", iv_expr);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }

    temp_char = (char *)xmlXPathCastToString(xpathObj);
    status = DtXMLIntervalSeconds(temp_char, &mysec);
    if (status > 0) {
        log_msg(config, LOG_ERR, "Error: unable to convert interval %s to seconds, error: %i\n", temp_char, status);
        StrFree(temp_char);
        return status;
    }
    else if (status == -1) {
        log_msg(config, LOG_INFO, "Warning: converting %s to seconds may not give what you expect\n", temp_char);
    }
    config->interval = mysec;
    log_msg(config, LOG_INFO, "Communication Interval: %i\n", config->interval);
    StrFree(temp_char);
    xmlXPathFreeObject(xpathObj);

    /* Evaluate xpath expression for backup interval */
    xpathObj = xmlXPathEvalExpression(bi_expr, xpathCtx);
    if(xpathObj == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", bi_expr);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }

    temp_char = (char *)xmlXPathCastToString(xpathObj);
    status = DtXMLIntervalSeconds(temp_char, &mysec);
    if (status > 0) {
        log_msg(config, LOG_ERR, "Error: unable to convert interval %s to seconds, error: %i\n", temp_char, status);
        StrFree(temp_char);
        return status;
    }
    else if (status == -1) {
        log_msg(config, LOG_INFO, "Warning: converting %s to seconds may not give what you expect\n", temp_char);
    }
    config->backupinterval = mysec;
    log_msg(config, LOG_INFO, "HSM Backup Interval: %i\n", config->backupinterval);
    StrFree(temp_char);
    xmlXPathFreeObject(xpathObj);

    /* Evaluate xpath expression for SQLite file location */
		
    xpathObj = xmlXPathEvalExpression(litexpr, xpathCtx);
    if(xpathObj == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", litexpr);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }
    if(xpathObj->nodesetval->nodeNr > 0) {
        db_found = SQLITE_DB;
        config->schema = xmlXPathCastToString(xpathObj);
        /*config->schema = NULL;
        temp_uchar = xmlXPathCastToString(xpathObj);
		StrAppend(&config->schema, temp_uchar);
        StrFree(temp_uchar); */
        log_msg(config, LOG_INFO, "SQLite database set to: %s\n", config->schema);
    }
    xmlXPathFreeObject(xpathObj);

    if (db_found == 0) {
        /* Get all of the MySQL stuff read in too */
        /* HOST */
        xpathObj = xmlXPathEvalExpression(mysql_host, xpathCtx);
		    if(xpathObj == NULL) {
		        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", mysql_host);
		        xmlXPathFreeContext(xpathCtx);
		        xmlFreeDoc(doc);
		        return(-1);
		    }
            if(xpathObj->nodesetval->nodeNr > 0) {
           db_found = MYSQL_DB;
        }
        config->host = xmlXPathCastToString(xpathObj);
        log_msg(config, LOG_INFO, "MySQL database host set to: %s\n", config->host);
        xmlXPathFreeObject(xpathObj);

        /* PORT */
        xpathObj = xmlXPathEvalExpression(mysql_port, xpathCtx);
        if(xpathObj == NULL) {
		        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", mysql_port);
		        xmlXPathFreeContext(xpathCtx);
		        xmlFreeDoc(doc);
		        return(-1);
		    }
            if(xpathObj->nodesetval->nodeNr > 0) {
            db_found = 0;
        }
        config->port = xmlXPathCastToString(xpathObj);
        log_msg(config, LOG_INFO, "MySQL database port set to: %s\n", config->port);
        xmlXPathFreeObject(xpathObj);

        /* SCHEMA */
        xpathObj = xmlXPathEvalExpression(mysql_db, xpathCtx);
        if(xpathObj == NULL) {
		        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", mysql_db);
		        xmlXPathFreeContext(xpathCtx);
		        xmlFreeDoc(doc);
		        return(-1);
		    }
            if(xpathObj->nodesetval->nodeNr > 0) {
            db_found = 0;
        }
        config->schema = xmlXPathCastToString(xpathObj);
        log_msg(config, LOG_INFO, "MySQL database schema set to: %s\n", config->schema);
        xmlXPathFreeObject(xpathObj);

        /* DB USER */
        xpathObj = xmlXPathEvalExpression(mysql_user, xpathCtx);
        if(xpathObj == NULL) {
		        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", mysql_user);
		        xmlXPathFreeContext(xpathCtx);
		        xmlFreeDoc(doc);
		        return(-1);
		    }
            if(xpathObj->nodesetval->nodeNr > 0) {
            db_found = 0;
        }
        config->user = xmlXPathCastToString(xpathObj);
        log_msg(config, LOG_INFO, "MySQL database user set to: %s\n", config->user);
        xmlXPathFreeObject(xpathObj);

        /* DB PASSWORD */
        xpathObj = xmlXPathEvalExpression(mysql_pass, xpathCtx);
        if(xpathObj == NULL) {
		        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", mysql_pass);
		        xmlXPathFreeContext(xpathCtx);
		        xmlFreeDoc(doc);
		        return(-1);
		    }
		    /* password may be blank */
        
        config->password = xmlXPathCastToString(xpathObj);
        log_msg(config, LOG_INFO, "MySQL database password set\n");
        xmlXPathFreeObject(xpathObj);

    }

    /* Check that we found one or the other database */
    if(db_found == 0) {
        log_msg(config, LOG_ERR, "Error: unable to find complete database connection expression in %s\n", filename);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }

    /* Check that we found the right database type */
    if (db_found != DbFlavour()) {
        log_msg(config, LOG_ERR, "Error: database in config file %s does not match libksm\n", filename);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }

    /* Evaluate xpath expression for log facility (user) */
    xpathObj = xmlXPathEvalExpression(log_user_expr, xpathCtx);
    if(xpathObj == NULL) {
        log_msg(config, LOG_ERR, "Error: unable to evaluate xpath expression: %s\n", log_user_expr);
        xmlXPathFreeContext(xpathCtx);
        xmlFreeDoc(doc);
        return(-1);
    }

    temp_char = (char *)xmlXPathCastToString(xpathObj);
    logFacilityName =  StrStrdup( temp_char );
    StrFree(temp_char);
    xmlXPathFreeObject(xpathObj);

    /* If nothing was found use the defaults, else set what we got */
    if (strlen(logFacilityName) == 0) {
        logFacilityName = StrStrdup( (char *)DEFAULT_LOG_FACILITY_STRING );
        config->log_user = DEFAULT_LOG_FACILITY;
        log_msg(config, LOG_INFO, "Using default log user: %s\n", logFacilityName);
    } else {
        status = get_log_user(logFacilityName, &my_log_user);
        if (status > 0) {
            log_msg(config, LOG_ERR, "Error: unable to set log user: %s, error: %i\n", logFacilityName, status);
            StrFree(logFacilityName);
            return status;
        }
        config->log_user = my_log_user;
        log_msg(config, LOG_INFO, "Log User set to: %s\n", logFacilityName);
    }

    log_switch(my_log_user, logFacilityName, config->program);

    /* Cleanup */
    /* TODO: some other frees are needed */
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);
    StrFree(logFacilityName);

    return(0);

}

/* To overcome the potential differences in sqlite compile flags assume that it is not
   happy with multiple connections.

   The following 2 functions take out a lock and release it
*/

int get_lite_lock(char *lock_filename, FILE* lock_fd)
{
    struct flock fl;
    struct timeval tv;

    if (lock_fd == NULL) {
        log_msg(NULL, LOG_ERR, "%s could not be opened\n", lock_filename);
        return 1;
    }

    memset(&fl, 0, sizeof(struct flock));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_pid = getpid();
    
    while (fcntl(fileno(lock_fd), F_SETLK, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            log_msg(NULL, LOG_INFO, "%s already locked, sleep\n", lock_filename);

            /* sleep for 10 seconds TODO make this configurable? */
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            select(0, NULL, NULL, NULL, &tv);

        } else {
            log_msg(NULL, LOG_INFO, "couldn't get lock on %s, %s\n", lock_filename, strerror(errno));
            return 1;
        }
    }

    return 0;

}

int release_lite_lock(FILE* lock_fd)
{
    struct flock fl;

    if (lock_fd == NULL) {
        return 1;
    }
    
    memset(&fl, 0, sizeof(struct flock));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;

    if (fcntl(fileno(lock_fd), F_SETLK, &fl) == -1) {
        return 1;
    }

    return 0;
}

/* convert the name of a log facility (user) into a number */
int get_log_user(const char* username, int* usernumber)
{
    if (username == NULL) {
        return 1;
    }
    /* Start with our default */
    *usernumber = DEFAULT_LOG_FACILITY;

    /* POSIX only specifies LOG_USER and LOG_LOCAL[0 .. 7] */
    if (strncasecmp(username, "user", 4) == 0) {
        *usernumber = LOG_USER;
    }
#ifdef LOG_KERN
    else if (strncasecmp(username, "kern", 4) == 0) {
        *usernumber = LOG_KERN;
    }
#endif  /* LOG_KERN */
#ifdef LOG_MAIL
    else if (strncasecmp(username, "mail", 4) == 0) {
        *usernumber = LOG_MAIL;
    }
#endif  /* LOG_MAIL */
#ifdef LOG_DAEMON
    else if (strncasecmp(username, "daemon", 6) == 0) {
        *usernumber = LOG_DAEMON;
    }
#endif  /* LOG_DAEMON */
#ifdef LOG_AUTH
    else if (strncasecmp(username, "auth", 4) == 0) {
        *usernumber = LOG_AUTH;
    }
#endif  /* LOG_AUTH */
#ifdef LOG_SYSLOG
    else if (strncasecmp(username, "syslog", 6) == 0) {
        *usernumber = LOG_SYSLOG;
    }
#endif  /* LOG_SYSLOG */
#ifdef LOG_LPR
    else if (strncasecmp(username, "lpr", 3) == 0) {
        *usernumber = LOG_LPR;
    }
#endif  /* LOG_LPR */
#ifdef LOG_NEWS
    else if (strncasecmp(username, "news", 4) == 0) {
        *usernumber = LOG_NEWS;
    }
#endif  /* LOG_NEWS */
#ifdef LOG_UUCP
    else if (strncasecmp(username, "uucp", 4) == 0) {
        *usernumber = LOG_UUCP;
    }
#endif  /* LOG_UUCP */
#ifdef LOG_AUDIT    /* Ubuntu at least doesn't want us to use LOG_AUDIT */
    else if (strncasecmp(username, "audit", 5) == 0) {
        *usernumber = LOG_AUDIT;
    }
#endif  /* LOG_AUDIT */
#ifdef LOG_CRON
    else if (strncasecmp(username, "cron", 4) == 0) {
        *usernumber = LOG_CRON;
    }
#endif  /* LOG_CRON */
    else if (strncasecmp(username, "local0", 6) == 0) {
        *usernumber = LOG_LOCAL0;
    }
    else if (strncasecmp(username, "local1", 6) == 0) {
        *usernumber = LOG_LOCAL1;
    }
    else if (strncasecmp(username, "local2", 6) == 0) {
        *usernumber = LOG_LOCAL2;
    }
    else if (strncasecmp(username, "local3", 6) == 0) {
        *usernumber = LOG_LOCAL3;
    }
    else if (strncasecmp(username, "local4", 6) == 0) {
        *usernumber = LOG_LOCAL4;
    }
    else if (strncasecmp(username, "local5", 6) == 0) {
        *usernumber = LOG_LOCAL5;
    }
    else if (strncasecmp(username, "local6", 6) == 0) {
        *usernumber = LOG_LOCAL6;
    }
    else if (strncasecmp(username, "local7", 6) == 0) {
        *usernumber = LOG_LOCAL7;
    }
    return 0;

}

