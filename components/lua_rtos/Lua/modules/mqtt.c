/*
 * Lua RTOS, Lua MQTT module
 *
 * Copyright (C) 2015 - 2017
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 *
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

#include "luartos.h"

#if CONFIG_LUA_RTOS_LUA_USE_MQTT
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "auxmods.h"
#include "modules.h"
#include "error.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <mqtt/MQTTClient.h>
#include <mqtt/MQTTClientPersistence.h>

#include <sys/mutex.h>
#include <sys/delay.h>
#include <sys/status.h>
#include <sys/syslog.h>
#include <sys/mount.h>

#define MQTT_MAX_RECONNECT_RETRIES 10

void MQTTClient_init();

// Module errors
#define LUA_MQTT_ERR_CANT_CREATE_CLIENT (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  0)
#define LUA_MQTT_ERR_CANT_SET_CALLBACKS (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  1)
#define LUA_MQTT_ERR_CANT_CONNECT       (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  2)
#define LUA_MQTT_ERR_CANT_SUBSCRIBE     (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  3)
#define LUA_MQTT_ERR_CANT_PUBLISH       (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  4)
#define LUA_MQTT_ERR_CANT_DISCONNECT    (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  5)
#define LUA_MQTT_ERR_LOST_CONNECTION    (DRIVER_EXCEPTION_BASE(MQTT_DRIVER_ID) |  6)

// Register driver and messages
DRIVER_REGISTER_BEGIN(MQTT,mqtt,NULL,NULL,NULL);
	DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotCreateClient, "can't create client", LUA_MQTT_ERR_CANT_CREATE_CLIENT);
	DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotSetCallbacks, "can't set callbacks", LUA_MQTT_ERR_CANT_SET_CALLBACKS);
	DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotConnect, "can't connect", LUA_MQTT_ERR_CANT_CONNECT);
	DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotSubscribeToTopic, "can't subscribe to topic", LUA_MQTT_ERR_CANT_SUBSCRIBE);
	DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotPublishToTopic, "can't publish to topic", LUA_MQTT_ERR_CANT_PUBLISH);
	DRIVER_REGISTER_ERROR(MQTT, mqtt, CannotDisconnect, "can't disconnect", LUA_MQTT_ERR_CANT_DISCONNECT);
	DRIVER_REGISTER_ERROR(MQTT, mqtt, LostConnection, "lost connection", LUA_MQTT_ERR_LOST_CONNECTION);
DRIVER_REGISTER_END(MQTT,mqtt,NULL,NULL,NULL);

static int client_inited = 0;

typedef struct {
    char *topic;
    int callback;
    void *next;
} mqtt_subs_callback;

typedef struct {
    lua_State *L;
    struct mtx callback_mtx;

    MQTTClient_connectOptions conn_opts;
    MQTTClient_SSLOptions ssl_opts;
    MQTTClient client;

    mqtt_subs_callback *callbacks;
    const char *ca_file;

    int secure;
    int persistence;
} mqtt_userdata;

static int add_subs_callback(mqtt_userdata *mqtt, const char *topic, int call) {
    mqtt_subs_callback *callback;

    // Create and populate callback structure
    callback = (mqtt_subs_callback *)malloc(sizeof(mqtt_subs_callback));
    if (!callback) {
        errno = ENOMEM;
        return -1;
    }

    callback->topic = (char *)malloc(strlen(topic) + 1);
    if (!callback->topic) {
        errno = ENOMEM;
        free(callback);
        return -1;
    }

    strcpy(callback->topic, topic);

    callback->callback = call;

    mtx_lock(&mqtt->callback_mtx);
    callback->next = mqtt->callbacks;
    mqtt->callbacks = callback;
    mtx_unlock(&mqtt->callback_mtx);

    return 0;
}

static int messageArrived(void *context, char * topicName, int topicLen, MQTTClient_message* m) {
    mqtt_userdata *mqtt = (mqtt_userdata *)context;
    if (mqtt) {

      mqtt_subs_callback *callback;
      int call = 0;

      mtx_lock(&mqtt->callback_mtx);

      callback = mqtt->callbacks;
      while (callback) {
          if (strcmp(callback->topic, topicName) == 0) {
              call = callback->callback;
              if (call != LUA_NOREF) {
                  lua_rawgeti(mqtt->L, LUA_REGISTRYINDEX, call);
                  lua_pushinteger(mqtt->L, m->payloadlen);
                  lua_pushlstring(mqtt->L, m->payload, m->payloadlen);
                  lua_call(mqtt->L, 2, 0);
              }
          }

          callback = callback->next;
      }

      mtx_unlock(&mqtt->callback_mtx);

      MQTTClient_freeMessage(&m);
      MQTTClient_free(topicName);

    }
    return 1;
}

static void connectionLost(void* context, char* cause) {
    mqtt_userdata *mqtt = (mqtt_userdata *)context;
    if (mqtt && mqtt->callback_mtx.sem) {
      mtx_lock(&mqtt->callback_mtx); //protect from being deleted by our own module

      int rc = -1;
      if (NETWORK_AVAILABLE()) {

        syslog(LOG_DEBUG, "mqtt: trying to reconnect\n");
        usleep(500 * 1000); //wait before trying to reconnect (this does only sleep the THREAD, NOT the cpu)
        for(int retries = 1; mqtt->client && rc < 0 && retries <= MQTT_MAX_RECONNECT_RETRIES; retries++) {
          rc = MQTTClient_connect(mqtt->client, &mqtt->conn_opts);
          if (rc < 0) {
            syslog(LOG_DEBUG, "mqtt: reconnect attempt %i: %i\n", retries, rc);
            usleep(500 * 1000); //wait before retrying (this does only sleep the THREAD, NOT the cpu)
          }
        }

      }

      if (rc < 0) {
        if (NETWORK_AVAILABLE()) {
          syslog(LOG_DEBUG, "mqtt: connection lost\n"); //reconnect didn't succeed
        }
        else {
          syslog(LOG_DEBUG, "mqtt: connection lost - no network available\n"); //reconnect not possible at all
        }
      }
      else {
        syslog(LOG_DEBUG, "mqtt: reconnected\n");
      }

      mtx_unlock(&mqtt->callback_mtx);
    }
}

// Lua: result = setup( id, clock )
static int lmqtt_client( lua_State* L ){
    int rc = 0;
    size_t lenClientId, lenHost;
    mqtt_userdata *mqtt;
    char url[250];

    const char *clientId = luaL_checklstring( L, 1, &lenClientId ); //is being strdup'd in MQTTClient_create
    const char *host = luaL_checklstring( L, 2, &lenHost ); //url is being strdup'd in MQTTClient_connectURI
    int port = luaL_checkinteger( L, 3 );

    luaL_checktype(L, 4, LUA_TBOOLEAN);
    int persistence = lua_toboolean( L, 4 ) ? MQTTCLIENT_PERSISTENCE_DEFAULT : MQTTCLIENT_PERSISTENCE_NONE;
    const char *persistence_folder = luaL_optstring( L, 5, NULL );

    luaL_checktype(L, 6, LUA_TBOOLEAN);
    int secure = lua_toboolean( L, 6 );

    const char *ca_file = luaL_optstring( L, 7, NULL );

    // Allocate mqtt structure and initialize
    mqtt = (mqtt_userdata *)lua_newuserdata(L, sizeof(mqtt_userdata));
    mqtt->L = L;
    mqtt->client = NULL;
    mqtt->callbacks = NULL;
    mqtt->secure = secure;
    mqtt->persistence = persistence;
    mqtt->ca_file = (ca_file ? strdup(ca_file):NULL); //save for use during mqtt_connect
    mtx_init(&mqtt->callback_mtx, NULL, NULL, 0);

    // needed for lmqtt_client_gc to not crash freeing the username and password
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    bcopy(&conn_opts, &mqtt->conn_opts, sizeof(MQTTClient_connectOptions));

    // Calculate uri
    snprintf(url, sizeof(url), "%s://%s:%d", mqtt->secure ? "ssl":"tcp", host, port);

    if (!client_inited) {
        MQTTClient_init();
        client_inited = 1;
    }

    //url is being strdup'd in MQTTClient_connectURI
    rc = MQTTClient_create(&mqtt->client, url, clientId, persistence, (char*)persistence_folder);
    if (rc < 0){
      return luaL_exception(L, LUA_MQTT_ERR_CANT_CREATE_CLIENT);
    }

    rc = MQTTClient_setCallbacks(mqtt->client, mqtt, connectionLost, messageArrived, NULL);
    if (rc < 0){
      return luaL_exception(L, LUA_MQTT_ERR_CANT_SET_CALLBACKS);
    }

    luaL_getmetatable(L, "mqtt.cli");
    lua_setmetatable(L, -2);

    return 1;
}

static int lmqtt_connected( lua_State* L ) {
    int rc;
    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *)luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    rc = MQTTClient_connected(mqtt->client);

    lua_pushboolean( L, rc == MQTTCLIENT_SUCCESS ? 1 : 0 );

    return 1;
}

static int lmqtt_connect( lua_State* L ) {
    int rc;
    int retries = 0;
    const char *user;
    const char *password;
    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *)luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    user = luaL_checkstring( L, 2 );
    password = luaL_checkstring( L, 3  );

    MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
    ssl_opts.trustStore = mqtt->ca_file; //has been strdup'd already
    ssl_opts.enableServerCertAuth = (ssl_opts.trustStore != NULL);
    bcopy(&ssl_opts, &mqtt->ssl_opts, sizeof(MQTTClient_SSLOptions));

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    conn_opts.connectTimeout = 4;
    conn_opts.keepAliveInterval = 60;
    conn_opts.reliable = 0;
    conn_opts.cleansession = 0;
    conn_opts.username = strdup(user); //needs to be strdup'd here for connectionLost usage
    conn_opts.password = strdup(password); // //needs to be strdup'd here for connectionLost usage
    conn_opts.ssl = &mqtt->ssl_opts;
    bcopy(&conn_opts, &mqtt->conn_opts, sizeof(MQTTClient_connectOptions));

retry:
    rc = MQTTClient_connect(mqtt->client, &mqtt->conn_opts);
    if (rc < 0){
      if (retries < 2) {
        retries++;
        goto retry;
      }

      return luaL_exception(L, LUA_MQTT_ERR_CANT_CONNECT);
    }

    return 0;
}

static int lmqtt_subscribe( lua_State* L ) {
    int rc;
    int qos;
    const char *topic;
    int callback = 0;

    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *)luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    topic = luaL_checkstring( L, 2 );
    qos = luaL_checkinteger( L, 3 );

    if (qos > 0 && mqtt->persistence == MQTTCLIENT_PERSISTENCE_NONE) {
      syslog(LOG_WARNING, "mqtt: please enable persistence for a qos > 0\n");
    }

    luaL_checktype(L, 4, LUA_TFUNCTION);
    // Copy argument (function) to the top of stack
    lua_pushvalue(L, 4);
    // Copy function reference
    callback = luaL_ref(L, LUA_REGISTRYINDEX);

    add_subs_callback(mqtt, topic, callback);

    rc = MQTTClient_subscribe(mqtt->client, topic, qos);
    if (rc == 0) {
        return 0;
    } else {
      return luaL_exception(L, LUA_MQTT_ERR_CANT_SUBSCRIBE);
    }
}

static int lmqtt_publish( lua_State* L ) {
    int rc;
    int qos;
    size_t payload_len;
    const char *topic;
    char *payload;

    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *)luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    topic = luaL_checkstring( L, 2 );
    payload = (char *)luaL_checklstring( L, 3, &payload_len );
    qos = luaL_checkinteger( L, 4 );

    if (qos > 0 && mqtt->persistence == MQTTCLIENT_PERSISTENCE_NONE) {
      syslog(LOG_WARNING, "mqtt: please enable persistence for a qos > 0\n");
    }

    rc = MQTTClient_publish(mqtt->client, topic, payload_len, payload,
            qos, 0, NULL);

    if (rc == 0) {
        return 0;
    } else {
      return luaL_exception(L, LUA_MQTT_ERR_CANT_PUBLISH);
    }
}

static int lmqtt_disconnect( lua_State* L ) {
    int rc = 0;

    mqtt_userdata *mqtt = NULL;

    mqtt = (mqtt_userdata *)luaL_checkudata(L, 1, "mqtt.cli");
    luaL_argcheck(L, mqtt, 1, "mqtt expected");

    if (MQTTClient_isConnected(mqtt->client)) {
      rc = MQTTClient_disconnect(mqtt->client, 0);
    }
    if (rc == 0) {
        return 0;
    } else {
      return luaL_exception(L, LUA_MQTT_ERR_CANT_DISCONNECT);
    }
}

// Destructor
static int lmqtt_client_gc (lua_State *L) {
    mqtt_userdata *mqtt = NULL;
    mqtt_subs_callback *callback;
    mqtt_subs_callback *nextcallback;

    mqtt = (mqtt_userdata *)luaL_testudata(L, 1, "mqtt.cli");
    if (mqtt && mqtt->callback_mtx.sem) {
        // Destroy callbacks
        mtx_lock(&mqtt->callback_mtx);

        callback = mqtt->callbacks;
        while (callback) {
            luaL_unref(L, LUA_REGISTRYINDEX, callback->callback);
            nextcallback = callback->next;

            free(callback);
            callback = nextcallback;
        }
        mqtt->callbacks = NULL;

        mtx_unlock(&mqtt->callback_mtx);

        // Disconnect and destroy client
        if (MQTTClient_isConnected(mqtt->client)) {
          MQTTClient_disconnect(mqtt->client, 0);
        }
        MQTTClient_destroy(&mqtt->client);
        mqtt->client = NULL;

        mtx_destroy(&mqtt->callback_mtx);
        //mtx_destroy does mqtt->callback_mtx.sem = 0;

        if (mqtt->ca_file) {
          free((char*)mqtt->ca_file);
          mqtt->ca_file = NULL;
          mqtt->ssl_opts.trustStore = NULL;
        }
        if (mqtt->conn_opts.username) {
          free((char*)mqtt->conn_opts.username);
          mqtt->conn_opts.username = NULL;
        }
        if (mqtt->conn_opts.password) {
          free((char*)mqtt->conn_opts.password);
          mqtt->conn_opts.password = NULL;
        }
    }

    return 0;
}

static const LUA_REG_TYPE lmqtt_map[] = {
  { LSTRKEY( "client"      ),   LFUNCVAL( lmqtt_client     ) },

  { LSTRKEY("QOS0"), LINTVAL(0) },
  { LSTRKEY("QOS1"), LINTVAL(1) },
  { LSTRKEY("QOS2"), LINTVAL(2) },

  { LSTRKEY("PERSISTENCE_FILE"), LINTVAL(MQTTCLIENT_PERSISTENCE_DEFAULT) },
  { LSTRKEY("PERSISTENCE_NONE"), LINTVAL(MQTTCLIENT_PERSISTENCE_NONE) },
  { LSTRKEY("PERSISTENCE_USER"), LINTVAL(MQTTCLIENT_PERSISTENCE_USER) },

  // Error definitions
  DRIVER_REGISTER_LUA_ERRORS(mqtt)
  { LNILKEY, LNILVAL }
};

static const LUA_REG_TYPE lmqtt_client_map[] = {
  { LSTRKEY( "connect"     ),   LFUNCVAL( lmqtt_connect    ) },
  { LSTRKEY( "connected"   ),   LFUNCVAL( lmqtt_connected  ) },
  { LSTRKEY( "disconnect"  ),   LFUNCVAL( lmqtt_disconnect ) },
  { LSTRKEY( "subscribe"   ),   LFUNCVAL( lmqtt_subscribe  ) },
  { LSTRKEY( "publish"     ),   LFUNCVAL( lmqtt_publish    ) },
  { LSTRKEY( "__metatable" ),   LROVAL  ( lmqtt_client_map ) },
  { LSTRKEY( "__index"     ),   LROVAL  ( lmqtt_client_map ) },
  { LSTRKEY( "__gc"        ),   LFUNCVAL( lmqtt_client_gc  ) },
  { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_mqtt( lua_State *L ) {
  luaL_newmetarotable(L,"mqtt.cli", (void *)lmqtt_client_map);

#if !LUA_USE_ROTABLE
  luaL_newlib(L, mqtt);
  return 1;
#else
  return 0;
#endif
}

MODULE_REGISTER_MAPPED(MQTT, mqtt, lmqtt_map, luaopen_mqtt);

#endif
