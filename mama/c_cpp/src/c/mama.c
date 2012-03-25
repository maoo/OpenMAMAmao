/* $Id: mama.c,v 1.128.4.7.2.2.4.19 2011/10/02 19:02:17 ianbell Exp $
 *
 * OpenMAMA: The open middleware agnostic messaging API
 * Copyright (C) 2011 NYSE Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "wombat/wincompat.h"
#include "wombat/environment.h"


#include <netdb.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>

#include "wombat/strutils.h"

#include <mama/mama.h>
#include <mama/error.h>
#include <mamainternal.h>
#include <mama/version.h>
#include <bridge.h>
#include <payloadbridge.h>
#include <property.h>
#include <platform.h>

#include "fileutils.h"
#include "reservedfieldsimpl.h"
#include <mama/statslogger.h>
#include <mama/stat.h>
#include <mama/statfields.h>
#include <statsgeneratorinternal.h>
#include <statsgeneratorinternal.h>
#include <mama/statscollector.h>

#define PROPERTY_FILE "mama.properties"
#define WOMBAT_PATH_ENV "WOMBAT_PATH"
#define MAMA_PROPERTY_BRIDGE "mama.bridge.provider"
#define DEFAULT_STATS_INTERVAL 60

#ifdef WITH_ENTITLEMENTS
#include <OeaClient.h>
#include <OeaStatus.h>
#define SERVERS_PROPERTY "entitlement.servers"
#define MAX_ENTITLEMENT_SERVERS 32
#define MAX_USER_NAME_STR_LEN 64
#define MAX_HOST_NAME_STR_LEN 64

extern void initReservedFields (void);


#if (OEA_MAJVERSION == 2 && OEA_MINVERSION >= 11) || OEA_MAJVERSION > 2

void WIN32_CB_FUNC_TYPE entitlementDisconnectCallback (oeaClient*,
                                    const OEA_DISCONNECT_REASON,
                                    const char * const,
                                    const char * const,
                                    const char * const);
void WIN32_CB_FUNC_TYPE entitlementUpdatedCallback (oeaClient*,
                                 int openSubscriptionForbidden);
void WIN32_CB_FUNC_TYPE entitlementCheckingSwitchCallback (oeaClient*,
                                        int isEntitlementsCheckingDisabled);
#else

void entitlementDisconnectCallback (oeaClient*,
                                    const OEA_DISCONNECT_REASON,
                                    const char * const,
                                    const char * const,
                                    const char * const);
void entitlementUpdatedCallback (oeaClient*,
                                 int openSubscriptionForbidden);
void entitlementCheckingSwitchCallback (oeaClient*,
                                        int isEntitlementsCheckingDisabled);
#endif
oeaClient *               gEntitlementClient = 0;
oeaStatus                 gEntitlementStatus;
mamaEntitlementCallbacks  gEntitlementCallbacks;
static const char*        gServerProperty     = NULL;
static const char*        gServers[MAX_ENTITLEMENT_SERVERS];
static mama_status enableEntitlements (const char **servers);
static const char*        gEntitled = "entitled";
#else
static const char*        gEntitled = "not entitled";
#endif /*WITH_ENTITLEMENTS */

int gGenerateQueueStats     = 0;
int gGenerateTransportStats = 0;
int gGenerateGlobalStats    = 0;
int gGenerateLbmStats       = 0;
int gLogQueueStats          = 0;
int gLogTransportStats      = 0;
int gLogGlobalStats         = 0;
int gLogLbmStats            = 0;
int gPublishQueueStats      = 0;
int gPublishTransportStats  = 0;
int gPublishGlobalStats     = 0;
int gPublishLbmStats        = 0;
int gCatchCallbackExceptions = 0;

static char gIPAddress[16];
static const char*  gUserName = NULL;
static const char*  gHostName = NULL;

static void lookupIPAddress (void);

static int gRefCount = 0;

wproperty_t             gProperties      = 0;
static mamaStatsLogger  gStatsPublisher  = NULL;

mamaStatsGenerator      gStatsGenerator         = NULL;
mamaStatsCollector*     gGlobalStatsCollector   = NULL;
mamaStat                gInitialStat;
mamaStat                gRecapStat;
mamaStat                gUnknownMsgStat;
mamaStat                gMessageStat;
mamaStat                gFtTakeoverStat;
mamaStat                gSubscriptionStat;
mamaStat                gTimeoutStat;
mamaStat                gWombatMsgsStat;
mamaStat                gFastMsgsStat;
mamaStat                gRvMsgsStat;

#define MAMA_PAYLOAD_MAX	CHAR_MAX

static mamaBridge       	gMamaBridges    [MAMA_MIDDLEWARE_MAX];
static mamaPayloadBridge    gMamaPayloads   [MAMA_PAYLOAD_MAX];
static mamaPayloadBridge    gDefaultPayload = NULL;

pthread_key_t last_err_key;

/**
 * struct mamaApplicationGroup
 * Contains the name of the application and its class name.
 */
typedef struct mamaAppContext_
{
    const char* myApplicationName;
    const char* myApplicationClass;
} mamaApplicationContext;


static mamaApplicationContext  appContext;
static char mama_ver_string[256];

/*  Description :   This function will free any memory associated with a
 *                  mamaApplicationContext object but will not free the
 *                  object itself.
 *  Arguments   :   context [I] The context object to free.
 */
void
mama_freeAppContext(mamaApplicationContext *context)
{
    /* Only continue if the object is valid. */
    if(context != NULL)
    {
        /* Free all memory */
        if(context->myApplicationName != NULL)
        {
            free((void *)context->myApplicationName);
            context->myApplicationName = NULL;
        }

        if(context->myApplicationClass != NULL)
        {
            free((void *)context->myApplicationClass);
            context->myApplicationClass = NULL;
        }
    }
}

mama_status
mama_setApplicationName (const char* applicationName)
{
    if (appContext.myApplicationName)
    {
        free ((void*)appContext.myApplicationName);
        appContext.myApplicationName = NULL;
    }

    if (applicationName)
    {
        appContext.myApplicationName = strdup (applicationName);
    }
    return MAMA_STATUS_OK;
}

mama_status
mama_setApplicationClassName (const char* className)
{
    if (appContext.myApplicationClass)
    {
        free ((void*)appContext.myApplicationClass);
        appContext.myApplicationClass = NULL;
    }

    if (className)
    {
        appContext.myApplicationClass =  strdup (className);
    }
    return MAMA_STATUS_OK;
}

mama_status
mama_getApplicationName (const char**  applicationName)
{
    if (applicationName == NULL) return MAMA_STATUS_NULL_ARG;
    *applicationName = appContext.myApplicationName;
    return MAMA_STATUS_OK;
}

mama_status
mama_getApplicationClassName (const char** className)
{
    if (className == NULL) return MAMA_STATUS_NULL_ARG;
    *className = appContext.myApplicationClass;
    return MAMA_STATUS_OK;
}

static void
mamaInternal_loadProperties (const char *path,
                             const char *filename)
{
    wproperty_t fileProperties;

    if( gProperties == 0 )
    {
        gProperties = properties_Create ();
    }

    if( !path )
    {
        path = getenv (WOMBAT_PATH_ENV);
        mama_log (MAMA_LOG_LEVEL_NORMAL, "Using path specified in %s",
            WOMBAT_PATH_ENV);
    }

    if( !filename )
    {
       filename = PROPERTY_FILE;
       mama_log (MAMA_LOG_LEVEL_NORMAL,
                 "Using default properties file %s",
                 PROPERTY_FILE);
    }

    mama_log (MAMA_LOG_LEVEL_NORMAL,
              "Attempting to load MAMA properties from %s", path ? path : "");

    fileProperties = properties_Load (path, filename);

    if( fileProperties == 0 )
    {
			mama_log (MAMA_LOG_LEVEL_ERROR, "Failed to open properties file.\n");
        return;
    }

    /* We've got file properties, so we need to merge 'em into
     * anything we've already gotten */
    properties_Merge( gProperties, fileProperties );

    /* Free the file properties, note that FreeEx2 is called to ensure that the data
     * isn't freed as the pointers have been copied over to gProperties.
     */
    properties_FreeEx2(fileProperties);
}

static int mamaInternal_statsPublishingEnabled ()
{
    return (gPublishGlobalStats
         || gPublishTransportStats
         || gPublishQueueStats
         || gPublishLbmStats);
}

static mama_status mamaInternal_loadStatsPublisher ()
{
    mamaBridge      bridge                  = NULL;
    mama_status     status                  = MAMA_STATUS_OK;
    const char*     statsLogMiddlewareName  = NULL;

    statsLogMiddlewareName = properties_Get (gProperties,
                                             "mama.statslogging.middleware");

    if (!statsLogMiddlewareName)
    {
        statsLogMiddlewareName = "wmw";
    }

    /* Will load the bridge if its not already loaded */
    if (MAMA_STATUS_OK !=
       (status = mama_loadBridge (&bridge, statsLogMiddlewareName)))
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mamaInternal_loadStatsLogger(): ",
                  "No bridge loaded for middleware [%s]",
                   statsLogMiddlewareName);
        return status;
    }

    return MAMA_STATUS_OK;
}

mama_status
mamaInternal_createStatsPublisher ()
{
    mama_status     result                  = MAMA_STATUS_OK;
    mamaBridge      bridge                  = NULL;
    mamaQueue       queue                   = NULL;
    mamaTransport   statsLogTport           = NULL;
    const char*     userName                = NULL;
    const char*     statsLogTportName       = NULL;
    const char*     statsLogMiddlewareName  = NULL;

    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats publishing enabled");

    statsLogTportName       = properties_Get (gProperties,
                                              "mama.statslogging.transport");
    statsLogMiddlewareName  = properties_Get (gProperties,
                                              "mama.statslogging.middleware");

    if (!statsLogMiddlewareName)
    {
        statsLogMiddlewareName = "wmw";
    }

    bridge = gMamaBridges[mamaMiddleware_convertFromString (statsLogMiddlewareName)];

    if (MAMA_STATUS_OK != (result = mamaBridgeImpl_getInternalEventQueue (bridge,
                                                                          &queue)))
    {
        return result;
    }

    result = mamaStatsLogger_allocate (&gStatsPublisher);

    if( result != MAMA_STATUS_OK )
        return result;

    mama_getUserName (&userName);
    lookupIPAddress();
    mamaStatsLogger_setReportSize       (gStatsPublisher, 100);
    mamaStatsLogger_setUserName         (gStatsPublisher, userName);
    mamaStatsLogger_setIpAddress        (gStatsPublisher, gIPAddress);
    mamaStatsLogger_setHostName         (gStatsPublisher, gHostName);
    mamaStatsLogger_setApplicationName  (gStatsPublisher,
                                         appContext.myApplicationName);
    mamaStatsLogger_setApplicationClass (gStatsPublisher,
                                         appContext.myApplicationClass);

    mamaStatsLogger_setLogMsgStats (gStatsPublisher, 0);

    if (!statsLogTportName)
    {
        statsLogTportName = "statslogger";
    }

    result = mamaTransport_allocate (&statsLogTport);
    if( result != MAMA_STATUS_OK )
        return result;

    result = mamaTransport_create (statsLogTport,
                                   statsLogTportName,
                                   bridge);
    if (result != MAMA_STATUS_OK)
        return result;

    result = mamaStatsLogger_createForStats (gStatsPublisher,
                                             queue,
                                             statsLogTport,
                                             STATS_TOPIC);
    if (result != MAMA_STATUS_OK)
        return result;

    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats logging middleware [%s]",
                                    statsLogMiddlewareName ? statsLogMiddlewareName : "");
    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats logging transport [%s]",
                                    statsLogTportName ? statsLogTportName : "");

    return result;
}

static mama_status
mamaInternal_enableStatsLogging ()
{
    mama_status     result                  = MAMA_STATUS_OK;
    const char*     statsLogIntervalStr     = NULL;

    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats logging enabled");

    if (mamaInternal_statsPublishingEnabled())
    {
        if (MAMA_STATUS_OK != (result = mamaInternal_createStatsPublisher ()))
        {
            mama_log (MAMA_LOG_LEVEL_ERROR,
                      "mamaInternal_enableStatsLogging(): "
                      "Could not create stats publisher");
            return result;
        }
    }

    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats logging interval [%s]",
                                    statsLogIntervalStr ? statsLogIntervalStr : "");
    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats logging: global stats logging %s",
                                     gGenerateGlobalStats ? "enabled" : "disabled");
    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats logging: transport stats logging %s",
                                     gGenerateTransportStats ? "enabled" : "disabled");
    mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats logging: queue stats logging %s",
                                     gGenerateQueueStats ? "enabled" : "disabled");


    if (gGenerateGlobalStats)
    {
        const char* appName;
        mama_getApplicationName (&appName);

        gGlobalStatsCollector = (mamaStatsCollector*)mamaStatsGenerator_allocateStatsCollector (gStatsGenerator);

        if (MAMA_STATUS_OK != (result =
            mamaStatsCollector_create (gGlobalStatsCollector,
                                       MAMA_STATS_COLLECTOR_TYPE_GLOBAL,
                                       appName,
                                       "-----")))
        {
            return result;
        }

        if (!gLogGlobalStats)
        {
            if (MAMA_STATUS_OK != (result =
                mamaStatsCollector_setLog (*gGlobalStatsCollector, 0)))
            {
                return MAMA_STATUS_OK;
            }
        }

        if (gPublishGlobalStats)
        {
            if (MAMA_STATUS_OK != (result =
                mamaStatsCollector_setPublish (*gGlobalStatsCollector, 1)))
            {
                return MAMA_STATUS_OK;
            }

            mama_log (MAMA_LOG_LEVEL_NORMAL, "Stats publishing enabled for global stats");
        }

        result = mamaStat_create (&gInitialStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatInitials.mName,
                                  MamaStatInitials.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gRecapStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatRecaps.mName,
                                  MamaStatRecaps.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gUnknownMsgStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatUnknownMsgs.mName,
                                  MamaStatUnknownMsgs.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gMessageStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatNumMessages.mName,
                                  MamaStatNumMessages.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gFtTakeoverStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatFtTakeovers.mName,
                                  MamaStatFtTakeovers.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gSubscriptionStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatNumSubscriptions.mName,
                                  MamaStatNumSubscriptions.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gTimeoutStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatTimeouts.mName,
                                  MamaStatTimeouts.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gWombatMsgsStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatWombatMsgs.mName,
                                  MamaStatWombatMsgs.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gFastMsgsStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatFastMsgs.mName,
                                  MamaStatFastMsgs.mFid);
        if (result != MAMA_STATUS_OK) return result;

        result = mamaStat_create (&gRvMsgsStat,
                                  gGlobalStatsCollector,
                                  MAMA_STAT_LOCKABLE,
                                  MamaStatRvMsgs.mName,
                                  MamaStatRvMsgs.mFid);
        if (result != MAMA_STATUS_OK) return result;

        mamaStatsGenerator_addStatsCollector (gStatsGenerator, gGlobalStatsCollector);
    }

    if (gLogQueueStats || gLogTransportStats || gLogGlobalStats)
    {
        mamaStatsGenerator_setLogStats (gStatsGenerator, 1);
    }
    else
    {
        mamaStatsGenerator_setLogStats (gStatsGenerator, 0);
    }

    if (gStatsPublisher != NULL)
    {
        if (MAMA_STATUS_OK != (result =
                        mamaStatsGenerator_setStatsLogger (gStatsGenerator, &gStatsPublisher)))
        {
            return result;
        }
    }

    return result;
}

mamaStatsGenerator
mamaInternal_getStatsGenerator()
{
    return gStatsGenerator;
}

mamaStatsCollector*
mamaInternal_getGlobalStatsCollector()
{
    return gGlobalStatsCollector;
}

/**
 * Expose the property object
 */
wproperty_t
mamaInternal_getProperties()
{
  return gProperties;
}

/**
 * Iterate through the bridge array and return the first
 * non-NULL value.
 */
mamaBridge
mamaInternal_findBridge ()
{
    int middleware = 0;
    mamaBridge bridge = NULL;

    for (middleware = 0; middleware < MAMA_MIDDLEWARE_MAX; middleware++)
    {
        bridge = gMamaBridges [middleware];

        if (bridge != NULL)
        {
            break;
        }
    }

    return bridge;
}

mamaPayloadBridge
mamaInternal_findPayload (char id)
{
    if (('\0' == id) || (MAMA_PAYLOAD_MAX < id)) return NULL;

    return gMamaPayloads[(uint8_t)id];
}

mamaPayloadBridge
mamaInternal_getDefaultPayload (void)
{
    return gDefaultPayload;
}

mama_status
mama_openWithProperties (const char* path,
                         const char* filename)
{
    mama_status     result			        = MAMA_STATUS_OK;
    mama_size_t     numBridges              = 0;
    mamaMiddleware  middleware              = 0;
    mamaQueue       statsGenQueue           = NULL;
    const char*     appString               = NULL;
    const char*     statsLogging            = "false";
	const char*		catchCallbackExceptions = NULL;

    if (pthread_key_create(&last_err_key, NULL) != 0)
    {
        mama_log (MAMA_LOG_LEVEL_NORMAL, "WARNING!!! - CANNOT ALLOCATE KEY FOR ERRORS");
    }

    /* If appName is not specified, set a default */
    appString = NULL;
    mama_getApplicationName(&appString);
    if (NULL==appString)
      mama_setApplicationName("MamaApplication");

    /* If appClass is not specified, set a default */
    appString = NULL;
    mama_getApplicationClassName(&appString);
    if (NULL==appString)
      mama_setApplicationClassName("MamaApplications");

#ifdef WITH_CALLBACK_RETURN
    mama_log (MAMA_LOG_LEVEL_WARN,
            "********************************************************");
    mama_log (MAMA_LOG_LEVEL_WARN, "WARNING!!! - In callback return mode."
                                      " Do not release!!!");
    mama_log (MAMA_LOG_LEVEL_WARN,
            "********************************************************");
#endif

#ifdef WITH_UNITTESTS
    mama_log (MAMA_LOG_LEVEL_WARN,
            "********************************************************");
    mama_log (MAMA_LOG_LEVEL_WARN, "WARNING!!! - Built for unittesting."
                                      " Do not release!!!");
    mama_log (MAMA_LOG_LEVEL_WARN,
            "********************************************************");
#endif

   if (gRefCount++)  return result;

#ifdef WITH_INACTIVE_CHECK
    mama_log (MAMA_LOG_LEVEL_WARN,
            "********************************************************");
    mama_log (MAMA_LOG_LEVEL_WARN, "WARNING!!! - In inactive subscription check mode."
                                      " Do not release!!!");
    mama_log (MAMA_LOG_LEVEL_WARN,
            "********************************************************");
#endif


#ifdef DEV_RELEASE
    mama_log (MAMA_LOG_LEVEL_WARN,
                "\n********************************************************************************\n"
                "Warning: This is a developer release and has only undergone basic sanity checks.\n"
                "It is for testing only and should not be used in a production environment\n"
                "**********************************************************************************");
#endif /* BAR_RELEASE */

    mamaInternal_loadProperties (path, filename);

    lookupIPAddress();
    initReservedFields();
    mama_loginit();

    /* Do not call mamaInternal_loadStatsPublisher here.
       It only needs to be called if we are publishing */

	catchCallbackExceptions = properties_Get (gProperties, "mama.catchcallbackexceptions.enable");
	if (catchCallbackExceptions != NULL && strtobool(catchCallbackExceptions))
	{
		gCatchCallbackExceptions = 1;
	}

    statsLogging = properties_Get (gProperties, "mama.statslogging.enable");

    if ( (statsLogging != NULL) && strtobool (statsLogging))
    {
        const char*     globalLogging           = NULL;
        const char*     globalPublishing        = NULL;
        const char*     transportLogging        = NULL;
        const char*     transportPublishing     = NULL;
        const char*     queueLogging            = NULL;
        const char*     queuePublishing         = NULL;
        const char*     lbmLogging              = NULL;
        const char*     lbmPublishing           = NULL;
        const char*     statsIntervalStr        = NULL;

        statsIntervalStr = properties_Get (gProperties,
                                        "mama.statslogging.interval");

        if (MAMA_STATUS_OK != (result = mamaStatsGenerator_create(
                                &gStatsGenerator,
                                statsIntervalStr ? atof (statsIntervalStr) : DEFAULT_STATS_INTERVAL)))
        {
            mama_log (MAMA_LOG_LEVEL_ERROR,
                      "mama_openWithProperties(): "
                      "Could not create stats generator.");
            return result;
        }


        globalLogging           = properties_Get (gProperties, "mama.statslogging.global.logging");
        globalPublishing        = properties_Get (gProperties, "mama.statslogging.global.publishing");
        transportLogging        = properties_Get (gProperties, "mama.statslogging.transport.logging");
        transportPublishing     = properties_Get (gProperties, "mama.statslogging.transport.publishing");
        queueLogging            = properties_Get (gProperties, "mama.statslogging.queue.logging");
        queuePublishing         = properties_Get (gProperties, "mama.statslogging.queue.publishing");
        lbmLogging              = properties_Get (gProperties, "mama.statslogging.lbm.logging");
        lbmPublishing           = properties_Get (gProperties, "mama.statslogging.lbm.publishing");

        /* If logging has been explicitly set false, and publishing is also set false, then don't
           generate stats (and neither log nor publish).

           If logging has not been specified (or has been set true), then generate and log stats.
           Publish stats if it has been set true. */
        if ( globalLogging != NULL && !(strtobool(globalLogging)) )
        {
            if ( globalPublishing != NULL && strtobool(globalPublishing) )
            {
                gGenerateGlobalStats = 1;
                gPublishGlobalStats  = 1;
            }
        }
        else
        {
            gGenerateGlobalStats = 1;
            gLogGlobalStats      = 1;

            if ( globalPublishing != NULL && strtobool(globalPublishing) )
            {
                gPublishGlobalStats = 1;
            }
        }

        if ( queueLogging != NULL && !(strtobool(queueLogging)) )
        {
            if ( queuePublishing != NULL && strtobool(queuePublishing) )
            {
                gGenerateQueueStats = 1;
                gPublishQueueStats  = 1;
            }
        }
        else
        {
            gGenerateQueueStats = 1;
            gLogQueueStats      = 1;

            if ( queuePublishing != NULL && strtobool(queuePublishing) )
            {
                gPublishQueueStats = 1;
            }
        }

        if ( transportLogging != NULL && !(strtobool(transportLogging)) )
        {
            if ( transportPublishing != NULL && strtobool(transportPublishing) )
            {
                gGenerateTransportStats = 1;
                gPublishTransportStats  = 1;
            }
        }
        else
        {
            gGenerateTransportStats = 1;
            gLogTransportStats      = 1;

            if ( transportPublishing != NULL && strtobool(transportPublishing) )
            {
                gPublishTransportStats = 1;
            }
        }

        if ( lbmLogging != NULL && !(strtobool(lbmLogging)) )
        {
            if ( lbmPublishing != NULL && strtobool(lbmPublishing) )
            {
                gGenerateLbmStats = 1;
                gPublishLbmStats  = 1;
            }
        }
        else
        {
            gGenerateLbmStats = 1;
            gLogLbmStats      = 1;

            if ( lbmPublishing != NULL && strtobool(lbmPublishing) )
            {
                gPublishLbmStats = 1;
            }
        }


        if (mamaInternal_statsPublishingEnabled())
        {
            mamaInternal_loadStatsPublisher();
        }
    }

    /* Look for a bridge for each of the middlewares and open them */
    for (middleware = 0; middleware != MAMA_MIDDLEWARE_MAX; ++middleware)
    {
        mamaBridgeImpl* impl = (mamaBridgeImpl*) gMamaBridges [middleware];
        if (impl)
        {
            mama_log (MAMA_LOG_LEVEL_FINE, mama_getVersion (gMamaBridges[middleware]));
            mamaQueue_enableStats(impl->mDefaultEventQueue);
            ++numBridges;
        }
    }

    if (0 == numBridges)
    {
        mama_log (MAMA_LOG_LEVEL_SEVERE,
                  "mama_openWithProperties(): "
                  "At least one bridge must be specified");
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    if (!gDefaultPayload)
    {


        mama_log (MAMA_LOG_LEVEL_SEVERE,
                  "mama_openWithProperties(): "
                  "At least one payload must be specified");
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

#ifndef WITH_ENTITLEMENTS
    mama_log (MAMA_LOG_LEVEL_WARN,
                "\n********************************************************************************\n"
                "Note: This build of the MAMA API is not enforcing entitlement checks.\n"
                "Please see the Licensing file for details\n"
                "**********************************************************************************");
#else
    result = enableEntitlements (NULL);
    if (result != MAMA_STATUS_OK)
    {
        mama_log (MAMA_LOG_LEVEL_SEVERE,
                  "mama_openWithProperties(): "
                  "Error connecting to Entitlements Server");
        mama_close();
        return result;
    }
#endif /* WITH_ENTITLEMENTS */

    if (strtobool(statsLogging))
    {
        /* No publishing, therefore no middleware needs to be specified
           in mama.properties.  Instead, check through loaded bridges */
        if (!mamaInternal_statsPublishingEnabled())
        {
            mamaBridgeImpl* impl = (mamaBridgeImpl*) mamaInternal_findBridge ();

            if (impl != NULL)
            {
                statsGenQueue = impl->mDefaultEventQueue;
            }
        }
        else
        {
            /* Stats publishing enabled, therefore use the mama.statslogging.middleware
               property */
            mamaBridge bridge;

            const char* statsMiddleware = NULL;
            statsMiddleware = properties_Get (gProperties, "mama.statslogging.middleware");

            if (!statsMiddleware)
            {
                statsMiddleware = "wmw";
            }

            bridge = gMamaBridges[mamaMiddleware_convertFromString (statsMiddleware)];

            if (MAMA_STATUS_OK != (result = mamaBridgeImpl_getInternalEventQueue (bridge,
                                                               &statsGenQueue)))
            {
                return result;
            }
        }

        if (MAMA_STATUS_OK != (result = mamaStatsGenerator_setQueue (gStatsGenerator, statsGenQueue)))
        {
            mama_log (MAMA_LOG_LEVEL_ERROR,
                      "mama_openWithProperties(): "
                      "Could not set queue for stats generator.");
            return result;
        }

        if (MAMA_STATUS_OK != (result=mamaInternal_enableStatsLogging()))
        {
            mama_log (MAMA_LOG_LEVEL_ERROR,
                      "mama_openWithProperties(): "
                      "Failed to enable stats logging");
            return result;
        }
    }

    return result;
}

mama_status
mama_open ()
{
    /*Passing NULL as path and filename will result in the
     default behaviour - mama.properties on $WOMBAT_PATH*/
    return mama_openWithProperties (NULL, NULL);
}

mama_status
mama_setProperty (const char* name,
                  const char* value)
{
    if (!gProperties)
    {
       mamaInternal_loadProperties( NULL, NULL );
    }

    if (!name||!value)
        return MAMA_STATUS_NULL_ARG;

    mama_log (MAMA_LOG_LEVEL_NORMAL,"Setting property: %s with value: %s",
                                   name, value);

    if( 0 == properties_setProperty (gProperties,
                                     name,
                                     value))
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
            "mama_setProperty(): Failed to set property");
        return MAMA_STATUS_PLATFORM;
    }
    return MAMA_STATUS_OK;
}

mama_status
mama_setPropertiesFromFile (const char *path,
                            const char *filename)
{
    wproperty_t fileProperties;

    if (!gProperties)
    {
       mamaInternal_loadProperties( NULL, NULL );
    }

    if( !path )
    {
        path = environment_getVariable(WOMBAT_PATH_ENV);
        mama_log (MAMA_LOG_LEVEL_NORMAL, "Using path specified in %s",
            WOMBAT_PATH_ENV);
    }

    if( !filename )
        return MAMA_STATUS_NULL_ARG;

    mama_log (MAMA_LOG_LEVEL_NORMAL,
              "Attempting to load additional MAMA properties from %s", path ? path : "");

    fileProperties = properties_Load (path, filename);

    if( fileProperties == 0 )
    {
        mama_log (MAMA_LOG_LEVEL_ERROR, "Failed to open additional properties file.\n");
        return MAMA_STATUS_IO_ERROR;
    }

    /* We've got file properties, so we need to merge 'em into
     * anything we've already gotten */
    properties_Merge( gProperties, fileProperties );

    /* Free the file properties, note that FreeEx2 is called to ensure that the data
     * isn't freed as the pointers have been copied over to gProperties.
     */
    properties_FreeEx2(fileProperties);

    return MAMA_STATUS_OK;
}

const char *
mama_getProperty (const char *name)
{
    if( !gProperties )
    {
        mamaInternal_loadProperties( NULL, NULL );
    }

    if( !name )
        return NULL;

    return properties_Get( gProperties, name );
}

const char*
mama_getVersion (mamaBridge bridgeImpl)
{
    mamaBridgeImpl* impl =  (mamaBridgeImpl*)bridgeImpl;

    if (!impl)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_getVersion(): No bridge implementation specified");
        return NULL;
    }

    /*Delegate the call to the bridge specific implementation*/
    snprintf(mama_ver_string,sizeof(mama_ver_string),"%s (%s) (%s)",
             mama_version, impl->bridgeGetVersion (), gEntitled);

    return mama_ver_string;
}

mama_status
mama_close ()
{
    mama_status    result     = MAMA_STATUS_OK;
    mamaMiddleware middleware = 0;
    int payload = 0;

    if( !--gRefCount )
    {
#ifdef WITH_ENTITLEMENTS
        if( gEntitlementClient != 0 )
        {
            oeaClient_destroy( gEntitlementClient );
            gEntitlementClient = 0;
        }
#endif /* WITH_ENTITLEMENTS */

        pthread_key_delete(last_err_key);

        for (middleware = 0; middleware != MAMA_MIDDLEWARE_MAX; ++middleware)
        {
            mamaBridge bridge = gMamaBridges[middleware];
            if (bridge)
            	mamaBridgeImpl_stopInternalEventQueue (bridge);
        }
        /* Look for a bridge for each of the payloads and close them */
        for (payload = 0; payload != MAMA_PAYLOAD_MAX; ++payload)
        {
        	mamaPayloadBridgeImpl* impl = (mamaPayloadBridgeImpl*) gMamaPayloads [(uint8_t)payload];
            if (impl)
            {

            }
            gMamaPayloads[(uint8_t)payload] = NULL;
        }

        gDefaultPayload = NULL;

        if (gInitialStat)
        {
            mamaStat_destroy (gInitialStat);
            gInitialStat = NULL;
        }

        if (gRecapStat)
        {
            mamaStat_destroy (gRecapStat);
            gRecapStat = NULL;
        }

        if (gUnknownMsgStat)
        {
            mamaStat_destroy (gUnknownMsgStat);
            gUnknownMsgStat = NULL;
        }


        if (gMessageStat)
        {
            mamaStat_destroy (gMessageStat);
            gMessageStat = NULL;
        }

        if (gFtTakeoverStat)
        {
            mamaStat_destroy (gFtTakeoverStat);
            gFtTakeoverStat = NULL;
        }

        if (gSubscriptionStat)
        {
            mamaStat_destroy (gSubscriptionStat);
            gSubscriptionStat = NULL;
        }

        if (gTimeoutStat)
        {
            mamaStat_destroy (gTimeoutStat);
            gTimeoutStat = NULL;
        }

        if (gWombatMsgsStat)
        {
            mamaStat_destroy (gWombatMsgsStat);
            gWombatMsgsStat = NULL;
        }

        if (gFastMsgsStat)
        {
            mamaStat_destroy (gFastMsgsStat);
            gFastMsgsStat = NULL;
        }

        if (gRvMsgsStat)
        {
            mamaStat_destroy (gRvMsgsStat);
            gRvMsgsStat = NULL;
        }

        if (gGlobalStatsCollector)
        {
            if (gStatsGenerator)
            {
                mamaStatsGenerator_removeStatsCollector (gStatsGenerator, gGlobalStatsCollector);
            }
            mamaStatsCollector_destroy (*gGlobalStatsCollector);
            gGlobalStatsCollector = NULL;
        }

        if (gStatsPublisher)
        {
            mamaStatsLogger_destroy (gStatsPublisher);
            gStatsPublisher = NULL;
        }

        cleanupReservedFields();

        if (gHostName)
            free ((void*)gHostName);

        /* Look for a bridge for each of the middlewares and close them */
        for (middleware = 0; middleware != MAMA_MIDDLEWARE_MAX; ++middleware)
        {
            mamaBridgeImpl* impl = (mamaBridgeImpl*) gMamaBridges [middleware];
            if (impl)
            {
                if (MAMA_STATUS_OK != (
                   (result = impl->bridgeClose (gMamaBridges[middleware]))))
                {
                    mama_log (MAMA_LOG_LEVEL_ERROR,
                              "mama_close(): Error closing %s bridge.",
                              mamaMiddleware_convertToString (middleware));

                }
            }
            gMamaBridges[middleware] = NULL;
        }

        /* The properties must not be closed down until after the bridges have been destroyed. */
        if (gProperties != 0)
        {
            properties_Free (gProperties);
            gProperties = 0;
        }

        /* Destroy the stats generator after the bridge is closed so we will
           have removed the default queue stats collector */
        if (gStatsGenerator)
        {
            mamaStatsGenerator_destroy (gStatsGenerator);
            gStatsGenerator = NULL;
        }

        /* Destroy logging */
        mama_logDestroy();

        /* Free application context details. */
        mama_freeAppContext(&appContext);

    }
    if (gRefCount < 0)
    {
        gRefCount = 0;
    }
    return result;
}

/**
 * Start processing messages.
 */
mama_status
mama_start (mamaBridge bridgeImpl)
{
    mamaBridgeImpl* impl =  (mamaBridgeImpl*)bridgeImpl;

    if (!impl)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_start(): No bridge implementation specified");
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    if (!impl->mDefaultEventQueue)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR, "mama_start(): NULL default queue. "
                  "Has mama_open() been called?");
        return MAMA_STATUS_INVALID_QUEUE;
    }

    /*Delegate to the bridge specific implementation*/
    return impl->bridgeStart (impl->mDefaultEventQueue);
}

struct startBackgroundClosure
{
    mamaStartCB mStartCallback;
    mamaBridge  mBridgeImpl;
};

static void* mamaStartThread (void* closure)
{
    /* size_t cast prevents compiler warning */
    struct startBackgroundClosure* cb =
                (struct startBackgroundClosure*)closure;
    mama_status rval = MAMA_STATUS_OK;

    if (!cb) return NULL;

    rval = mama_start (cb->mBridgeImpl);

    cb->mStartCallback (rval);

    /* Free the closure object */
    free(cb);

    return NULL;
}

mama_status
mama_startBackground (mamaBridge   bridgeImpl,
                      mamaStartCB callback)
{
    struct startBackgroundClosure*  closureData;
    wthread_t       t = 0;

    if (!bridgeImpl)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR, "mama_startBackground(): NULL bridge "
                  " impl.");
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    if (!callback)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR, "mama_startBackground(): No "
                  "callback specified.");
        return MAMA_STATUS_INVALID_ARG;
    }

    closureData = calloc (1, (sizeof (struct startBackgroundClosure)));

    if (!closureData)
        return MAMA_STATUS_NOMEM;

    closureData->mStartCallback = callback;
    closureData->mBridgeImpl    = bridgeImpl;

    if (0 != wthread_create(&t, NULL, mamaStartThread, (void*) closureData))
    {
        mama_log (MAMA_LOG_LEVEL_ERROR, "Could not start background MAMA "
                  "thread.");
        return MAMA_STATUS_SYSTEM_ERROR;
    }

    return MAMA_STATUS_OK;
}

/**
 * Stop processing messages
 */
mama_status
mama_stop (mamaBridge bridgeImpl)
{
    mamaBridgeImpl* impl =  (mamaBridgeImpl*)bridgeImpl;

    if (!impl)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_stop(): No bridge implementation specified");
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    if (!impl->mDefaultEventQueue)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_stop(): NULL default queue. Has mama_open() been "
                  "called?");
        return MAMA_STATUS_INVALID_QUEUE;
    }

    /*Delegate to the bridge specific implementation*/
    return impl->bridgeStop (impl->mDefaultEventQueue);
}

/**
 * Stops all the bridges.
 */
mama_status
mama_stopAll (void)
{
    mama_status result = MAMA_STATUS_OK;
    mama_status status = MAMA_STATUS_OK;
    mamaMiddleware middleware;
    /* Look for a bridge for each of the middlewares and open them */
    for (middleware = 0; middleware != MAMA_MIDDLEWARE_MAX; ++middleware)
    {
        mamaBridgeImpl* impl = (mamaBridgeImpl*) gMamaBridges [middleware];
        if (impl)
        {
            status = mama_stop (gMamaBridges[middleware]);
            if (MAMA_STATUS_OK != status)
            {
                mama_log (MAMA_LOG_LEVEL_ERROR,
                    "mama_stopAll(): error stopping %s bridge",
                    mamaMiddleware_convertToString (middleware));
                    result = status;
            }
        }
    }
    return result;
}

mama_status
mama_getDefaultEventQueue (mamaBridge bridgeImpl,
                           mamaQueue* defaultQueue)
{
    mamaBridgeImpl* impl =  (mamaBridgeImpl*)bridgeImpl;

    if (!impl)
    {
        mama_log (MAMA_LOG_LEVEL_WARN, "mama_getDefaultEventQueue(): "
                  "No bridge implementation specified");
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    if (!impl->mDefaultEventQueue)
    {
        mama_log (MAMA_LOG_LEVEL_WARN, "mama_getDefaultEventQueue (): "
                  "NULL default queue for bridge impl. Has mama_open() been "
                  "called?");
        return MAMA_STATUS_INVALID_QUEUE;
    }

    *defaultQueue = impl->mDefaultEventQueue;
    return MAMA_STATUS_OK;
}

mama_status
mama_getUserName (const char** userName)
{
    if (userName == NULL)
        return MAMA_STATUS_NULL_ARG;

    if (!gUserName)
    {
        gUserName = strdup (getpwuid(getuid())->pw_name);
    }

    *userName = gUserName;
    return MAMA_STATUS_OK;
}

mama_status
mama_getHostName (const char** hostName)
{
    if (hostName == NULL) return MAMA_STATUS_NULL_ARG;
    *hostName = gHostName;
    return MAMA_STATUS_OK;
}

mama_status
mama_getIpAddress (const char** ipAddress)
{
    if (ipAddress == NULL) return MAMA_STATUS_NULL_ARG;
    *ipAddress = gIPAddress;
    return MAMA_STATUS_OK;
}

static void
lookupIPAddress (void)
{
    struct hostent *host = NULL;
    char           *addrStr = "not determined";

    struct         utsname uts;
    memset( gIPAddress, 0, 16 );
    uname (&uts);
    gHostName = strdup (uts.nodename);

    host = gethostbyname( gHostName );

    if( gHostName == NULL ||
        host == NULL      ||
        host->h_addr_list[0] == NULL )
    {
       strncpy( (char *)gIPAddress, "not determined", sizeof( gIPAddress ) );
    }
    else
    {
        addrStr = inet_ntoa( *((struct in_addr *)( host->h_addr_list[0] )));
    }

    strncpy ((char*)gIPAddress, addrStr, sizeof (gIPAddress));
}

#ifdef WITH_ENTITLEMENTS

mama_status
mama_registerEntitlementCallbacks (const mamaEntitlementCallbacks* entitlementCallbacks)
{
    if (entitlementCallbacks == NULL) return MAMA_STATUS_NULL_ARG;
    gEntitlementCallbacks = *entitlementCallbacks;
    return MAMA_STATUS_OK;
}

#if (OEA_MAJVERSION == 2 && OEA_MINVERSION >= 11) || OEA_MAJVERSION > 2
void WIN32_CB_FUNC_TYPE entitlementDisconnectCallback (oeaClient*                  client,
                                    const OEA_DISCONNECT_REASON reason,
                                    const char * const          userId,
                                    const char * const          host,
                                    const char * const          appName)
{
    if (gEntitlementCallbacks.onSessionDisconnect != NULL)
    {
        gEntitlementCallbacks.onSessionDisconnect (reason, userId, host, appName);
    }
}

void WIN32_CB_FUNC_TYPE entitlementUpdatedCallback (oeaClient* client,
                                 int openSubscriptionForbidden)
{
    if (gEntitlementCallbacks.onEntitlementUpdate != NULL)
    {
        gEntitlementCallbacks.onEntitlementUpdate();
    }
}

void WIN32_CB_FUNC_TYPE entitlementCheckingSwitchCallback (oeaClient* client,
                                        int isEntitlementsCheckingDisabled)
{
    if (gEntitlementCallbacks.onEntitlementCheckingSwitch != NULL)
    {
        gEntitlementCallbacks.onEntitlementCheckingSwitch(isEntitlementsCheckingDisabled);
    }
}

#else

void entitlementDisconnectCallback (oeaClient*                  client,
                                    const OEA_DISCONNECT_REASON reason,
                                    const char * const          userId,
                                    const char * const          host,
                                    const char * const          appName)
{
    if (gEntitlementCallbacks.onSessionDisconnect != NULL)
    {
        gEntitlementCallbacks.onSessionDisconnect (reason, userId, host, appName);
    }
}

void entitlementUpdatedCallback (oeaClient* client,
                                 int openSubscriptionForbidden)
{
    if (gEntitlementCallbacks.onEntitlementUpdate != NULL)
    {
        gEntitlementCallbacks.onEntitlementUpdate();
    }
}

void entitlementCheckingSwitchCallback (oeaClient* client,
                                        int isEntitlementsCheckingDisabled)
{
    if (gEntitlementCallbacks.onEntitlementCheckingSwitch != NULL)
    {
        gEntitlementCallbacks.onEntitlementCheckingSwitch(isEntitlementsCheckingDisabled);
    }
}

#endif


const char **
mdrvImpl_ParseServersProperty()
{
    char *ptr;
    int idx = 0;

    if (gServerProperty == NULL)
    {
        memset (gServers, 0, sizeof(gServers));

        if( properties_Get (gProperties, SERVERS_PROPERTY) == NULL)
        {
            if (gMamaLogLevel)
            {
                mama_log( MAMA_LOG_LEVEL_WARN,
                          "Failed to open properties file "
                          "or no entitlement.servers property." );
            }
            return NULL;
        }

        gServerProperty = strdup (properties_Get (gProperties,
                                                  SERVERS_PROPERTY));

        if (gMamaLogLevel)
        {
            mama_log (MAMA_LOG_LEVEL_NORMAL,
                      "entitlement.servers=%s",
                      gServerProperty == NULL ? "NULL" : gServerProperty);
        }

        while( idx < MAX_ENTITLEMENT_SERVERS - 1 )
        {
            gServers[idx] = strtok_r (idx == 0 ? (char *)gServerProperty : NULL
                                      , ",",
                                      &ptr);


            if (gServers[idx++] == NULL) /* last server parsed */
            {
                break;
            }

            if (gMamaLogLevel)
            {
                mama_log (MAMA_LOG_LEVEL_NORMAL,
                          "Parsed entitlement server: %s",
                          gServers[idx-1]);
            }
        }
    }
    return gServers;
}

static mama_status
enableEntitlements (const char **servers)
{
    int size = 0;
    const char* portLowStr = NULL;
    const char* portHighStr = NULL;
    int portLow = 8000;
    int portHigh = 8001;
    oeaCallbacks entitlementCallbacks;
    const char* altUserId;
    const char* altIp;
    const char* site;


    if (gEntitlementClient != 0)
    {
        oeaClient_destroy (gEntitlementClient);
        gEntitlementClient = 0;
    }

    if (servers == NULL)
    {
        if (NULL == (servers = mdrvImpl_ParseServersProperty()))
        {
            return MAMA_ENTITLE_NO_SERVERS_SPECIFIED;
        }
    }

    while (servers[size] != NULL)
    {
        size = size + 1;
    }

    mama_log (MAMA_LOG_LEVEL_NORMAL,
              "Attempting to connect to entitlement server");

    portLowStr  = properties_Get (gProperties, "mama.entitlement.portlow");
    portHighStr = properties_Get (gProperties, "mama.entitlement.porthigh");

    /*properties_Get returns NULL if property does not exist, in which case
      we just use defaults*/
    if (portLowStr != NULL)
    {
        portLow  = (int)atof(portLowStr);
    }

    if (portHighStr != NULL)
    {
        portHigh = (int)atof(portHighStr);
    }

    altUserId   = properties_Get (gProperties, "mama.entitlement.altuserid");
    site = properties_Get (gProperties, "mama.entitlement.site");
    altIp = properties_Get (gProperties, "mama.entitlement.effective_ip_address");
    entitlementCallbacks.onDisconnect = entitlementDisconnectCallback;
    entitlementCallbacks.onEntitlementsUpdated = entitlementUpdatedCallback;
    entitlementCallbacks.onSwitchEntitlementsChecking = entitlementCheckingSwitchCallback;

    gEntitlementClient = oeaClient_create(&gEntitlementStatus,
                                site,
                                portLow,
                                portHigh,
                                servers,
                                size);

    if (gEntitlementStatus != OEA_STATUS_OK)
    {
        return gEntitlementStatus + MAMA_STATUS_BASE;
    }

    if (gEntitlementClient != 0)
    {
        if (OEA_STATUS_OK != (gEntitlementStatus = oeaClient_setCallbacks (gEntitlementClient, &entitlementCallbacks)))
        {
            return gEntitlementStatus + MAMA_STATUS_BASE;
        }

        if (OEA_STATUS_OK != (gEntitlementStatus = oeaClient_setAlternativeUserId (gEntitlementClient, altUserId)))
        {
            return gEntitlementStatus + MAMA_STATUS_BASE;
        }

        if (OEA_STATUS_OK != (gEntitlementStatus = oeaClient_setEffectiveIpAddress (gEntitlementClient, altIp)))
        {
            return gEntitlementStatus + MAMA_STATUS_BASE;
        }

        if (OEA_STATUS_OK != (gEntitlementStatus = oeaClient_setApplicationId (gEntitlementClient, appContext.myApplicationName)))
        {
            return gEntitlementStatus + MAMA_STATUS_BASE;
        }

        if (OEA_STATUS_OK != (gEntitlementStatus = oeaClient_downloadEntitlements ((oeaClient*const)gEntitlementClient)))
        {
            return gEntitlementStatus + MAMA_STATUS_BASE;
        }
    }

    return MAMA_STATUS_OK;
}

#endif


void
mamaInternal_registerBridge (mamaBridge     bridge,
                             const char*    middlewareName)
{
    mamaMiddleware middleware =
                    mamaMiddleware_convertFromString (middlewareName);
    if (middleware >= MAMA_MIDDLEWARE_MAX)
    {
        mama_log (MAMA_LOG_LEVEL_SEVERE,
                  "mamaInternal_registerBridge(): Invalid middleware [%s]",
                  middlewareName ? middlewareName : "");
        return;
    }

    gMamaBridges [middleware] = bridge;
}

mama_status
mama_setDefaultPayload (char id)
{
    if (('\0' == id) || (MAMA_PAYLOAD_MAX < id) || gMamaPayloads[(uint8_t)id] == NULL) return MAMA_STATUS_NULL_ARG;

    gDefaultPayload = gMamaPayloads[(uint8_t)id];

    return MAMA_STATUS_OK;
}
mama_status
mama_loadPayloadBridge (mamaPayloadBridge* impl,
                        const char*        payloadName)
{
    char                    bridgeImplName  [256];
    char                    initFuncName    [256];
    LIB_HANDLE              bridgeLib       = NULL;
    msgPayload_createImpl   initFunc        = NULL;
    mama_status             status          = MAMA_STATUS_OK;
    char                    payloadChar;

    if (!impl || !payloadName)
        return MAMA_STATUS_NULL_ARG;

    snprintf (bridgeImplName, 256, "mama%simpl",
              payloadName);

    bridgeLib = openSharedLib (bridgeImplName, NULL);

    if (!bridgeLib)
    {

       mama_log (MAMA_LOG_LEVEL_ERROR,
                "mama_loadPayloadBridge(): "
                "Could not open payload bridge library [%s] [%s]",
                 bridgeImplName ? bridgeImplName : "",
                 getLibError());
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    snprintf (initFuncName, 256, "%sPayload_createImpl",  payloadName);

    /* Gives a warning - casting from void* to bridge_createImpl func */
    initFunc  = (msgPayload_createImpl) loadLibFunc (bridgeLib, initFuncName);

    if (!initFunc)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_loadPayloadBridge(): "
                  "Could not find function [%s] in library [%s]",
                   initFuncName ? initFuncName : "",
                   bridgeImplName ? bridgeImplName : "");
        closeSharedLib (bridgeLib);
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    if (MAMA_STATUS_OK != (status = initFunc (impl, &payloadChar)))
    {
        return status;
    }

    if (!impl)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_loadPayloadBridge(): Error in [%s] ", initFuncName);
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    if (gMamaPayloads [payloadChar])
    {
        mama_log (MAMA_LOG_LEVEL_NORMAL,
             "mama_loadPayloadBridge(): "
             "Payload bridge %s already loaded",
             payloadName);
            return MAMA_STATUS_OK;
    }

    gMamaPayloads [payloadChar] = *impl;

    if (!gDefaultPayload)
    {
        gDefaultPayload = *impl;
    }

    mama_log (MAMA_LOG_LEVEL_NORMAL,
             "mama_loadPayloadBridge(): "
             "Sucessfully loaded %s payload bridge from library [%s]",
             payloadName, bridgeImplName);

    return MAMA_STATUS_OK;
}

int
mamaInternal_generateLbmStats ()
{
	return gGenerateLbmStats;
}

mama_status
mama_loadBridge (mamaBridge* impl,
                 const char* middlewareName)
{
	/* Otherwise this is a dynamic build, load the bridge normally. */
	return mama_loadBridgeWithPath (impl, middlewareName, NULL);
}

mama_status
mama_loadBridgeWithPath (mamaBridge* impl,
                         const char* middlewareName,
                         const char* path)
{
    char                bridgeImplName  [256];
    char                initFuncName    [256];
    LIB_HANDLE          bridgeLib       = NULL;
    bridge_createImpl   initFunc        = NULL;
    char*				payloadName		= NULL;
    char				payloadId		= NULL;
    mama_status 		result			= MAMA_STATUS_OK;
    mamaMiddleware      middleware      =
                    mamaMiddleware_convertFromString (middlewareName);

    if (!impl)
        return MAMA_STATUS_NULL_ARG;

    if (middleware >= MAMA_MIDDLEWARE_MAX)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_loadBridge(): Invalid middleware [%s]",
                  middlewareName);
    }

    /* Check if a bridge has already been initialized for the middleware */
    if (gMamaBridges [middleware])
    {
        *impl = gMamaBridges [middleware];
        return MAMA_STATUS_OK;
    }


    snprintf (bridgeImplName, 256, "mama%simpl",
              middlewareName);

    bridgeLib = openSharedLib (bridgeImplName, path);

    if (!bridgeLib)
    {

        if (path)
        {
                mama_log (MAMA_LOG_LEVEL_ERROR,
                "mama_loadmamaPayload(): "
                "Could not open middleware bridge library [%s] [%s] [%s]",
                path,
                bridgeImplName ? bridgeImplName : "",
                getLibError());
        }

        else
        {
                mama_log (MAMA_LOG_LEVEL_ERROR,
                "mama_loadmamaPayload(): "
                "Could not open middleware bridge library [%s] [%s]",
                bridgeImplName ? bridgeImplName : "",
                getLibError());
        }
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    snprintf (initFuncName, 256, "%sBridge_createImpl",  middlewareName);

    /* Gives a warning - casting from void* to bridge_createImpl func */
    initFunc  = (bridge_createImpl) loadLibFunc (bridgeLib, initFuncName);

    if (!initFunc)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_loadBridge(): "
                  "Could not find function [%s] in library [%s]",
                   initFuncName ? initFuncName : "",
                   bridgeImplName ? bridgeImplName : "");
        closeSharedLib (bridgeLib);
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    initFunc (impl);

    if (!impl)
    {
        mama_log (MAMA_LOG_LEVEL_ERROR,
                  "mama_loadBridge(): Error in [%s] ", initFuncName);
        return MAMA_STATUS_NO_BRIDGE_IMPL;
    }

    mama_log (MAMA_LOG_LEVEL_NORMAL,
             "mama_loadBridge(): "
             "Sucessfully loaded %s bridge from library [%s]",
             middlewareName, bridgeImplName);


    result = ((mamaBridgeImpl*)(*impl))->bridgeOpen (*impl);

    if (MAMA_STATUS_OK != result) return result;

    if (((mamaBridgeImpl*)(*impl))->bridgeGetDefaultPayloadId(&payloadName, &payloadId) == MAMA_STATUS_OK)
    {
		if (!gMamaPayloads [(uint8_t)payloadId])
		{
			mamaPayloadBridge payloadImpl;
			mama_loadPayloadBridge (&payloadImpl,payloadName);
		}
    }

    gMamaBridges [middleware] = *impl;

    return MAMA_STATUS_OK;
}

/*
 * Function pointer type for calling getVersion in the wrapper
 */
typedef const char* (MAMACALLTYPE *fpWrapperGetVersion)(void);

static fpWrapperGetVersion wrapperGetVersion = NULL;

MAMAExpDLL
void
mama_setWrapperGetVersion(fpWrapperGetVersion value)
{
	wrapperGetVersion = value;
}

/**
* Exposes the property for whether or not the test classes need to be used for catching callback exceptions
*/
int
mamaInternal_getCatchCallbackExceptions (void)
{
	return gCatchCallbackExceptions;
}


/* Do not expose in the public headers */
const char*
mama_wrapperGetVersion(mamaBridge bridge)
{
  if (wrapperGetVersion)
	  /* use getVersion from wrapper */
	  return wrapperGetVersion();
  else
	  /* Use native getVersion */
	  return mama_getVersion(bridge);
}

void
mama_setLastError (mamaError error)
{
     pthread_setspecific(last_err_key, (void*)error);
}

mamaError
mama_getLastErrorCode (void)
{
    return (mamaError)pthread_getspecific(last_err_key);
}

const char*
mama_getLastErrorText (void)
{
    return mamaError_convertToString((mamaError)pthread_getspecific(last_err_key));
}


mama_status
mama_setBridgeInfoCallback (mamaBridge bridgeImpl, bridgeInfoCallback callback)
{
    mamaBridgeImpl* impl = (mamaBridgeImpl*)bridgeImpl;

    switch (mamaMiddleware_convertFromString (impl->bridgeGetName()))
    {
        case MAMA_MIDDLEWARE_LBM    :   impl->mBridgeInfoCallback = callback;
                                        return MAMA_STATUS_OK;
        case MAMA_MIDDLEWARE_TIBRV  :
        case MAMA_MIDDLEWARE_WMW    :
        default                     :   return MAMA_STATUS_NOT_IMPLEMENTED;
    }

    return MAMA_STATUS_OK;
}