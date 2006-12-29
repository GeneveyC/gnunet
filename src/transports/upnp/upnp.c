/**
 * @file upnp.c UPnP Implementation
 * @ingroup core
 *
 * gaim
 *
 * Gaim is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "platform.h"
#include "xmlnode.h"
#include "util.h"
#include "upnp.h"
#include "error.h"
#include "ip.h"

#include <curl/curl.h>

/***************************************************************
** General Defines                                             *
****************************************************************/
#define HTTP_OK "200 OK"
#define DEFAULT_HTTP_PORT 80
#define DISCOVERY_TIMEOUT 1000

/***************************************************************
** Discovery/Description Defines                               *
****************************************************************/
#define NUM_UDP_ATTEMPTS 2

/* Address and port of an SSDP request used for discovery */
#define HTTPMU_HOST_ADDRESS "239.255.255.250"
#define HTTPMU_HOST_PORT 1900

#define SEARCH_REQUEST_DEVICE "urn:schemas-upnp-org:service:%s"

#define SEARCH_REQUEST_STRING \
	"M-SEARCH * HTTP/1.1\r\n" \
	"MX: 2\r\n" \
	"HOST: 239.255.255.250:1900\r\n" \
	"MAN: \"ssdp:discover\"\r\n" \
	"ST: urn:schemas-upnp-org:service:%s\r\n" \
	"\r\n"

#define WAN_IP_CONN_SERVICE "WANIPConnection:1"
#define WAN_PPP_CONN_SERVICE "WANPPPConnection:1"

/******************************************************************
** Action Defines                                                 *
*******************************************************************/

#define HTTP_POST_SOAP_ACTION \
	"SOAPACTION: \"urn:schemas-upnp-org:service:%s#%s\"\r\n"	\
	"CONTENT-TYPE: text/xml ; charset=\"utf-8\"\r\n" \
	"CONTENT-LENGTH: %" G_GSIZE_FORMAT "\r\n\r\n%s"

#define SOAP_ACTION \
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n" \
	"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" " \
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n" \
	  "<s:Body>\r\n" \
	    "<u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s\">\r\n" \
	      "%s" \
	    "</u:%s>\r\n" \
	  "</s:Body>\r\n" \
	"</s:Envelope>"

#define PORT_MAPPING_LEASE_TIME "0"
#define PORT_MAPPING_DESCRIPTION "GNUNET_UPNP_PORT_FORWARD"

#define ADD_PORT_MAPPING_PARAMS \
	"<NewRemoteHost></NewRemoteHost>\r\n" \
	"<NewExternalPort>%i</NewExternalPort>\r\n" \
	"<NewProtocol>%s</NewProtocol>\r\n" \
	"<NewInternalPort>%i</NewInternalPort>\r\n" \
	"<NewInternalClient>%s</NewInternalClient>\r\n" \
	"<NewEnabled>1</NewEnabled>\r\n" \
	"<NewPortMappingDescription>" \
	PORT_MAPPING_DESCRIPTION \
	"</NewPortMappingDescription>\r\n" \
	"<NewLeaseDuration>" \
	PORT_MAPPING_LEASE_TIME \
	"</NewLeaseDuration>\r\n"

#define DELETE_PORT_MAPPING_PARAMS \
	"<NewRemoteHost></NewRemoteHost>\r\n" \
	"<NewExternalPort>%i</NewExternalPort>\r\n" \
	"<NewProtocol>%s</NewProtocol>\r\n"

typedef enum {
  GAIM_UPNP_STATUS_UNDISCOVERED = -1,
  GAIM_UPNP_STATUS_UNABLE_TO_DISCOVER,
  GAIM_UPNP_STATUS_DISCOVERING,
  GAIM_UPNP_STATUS_DISCOVERED
} GaimUPnPStatus;

typedef struct {
  GaimUPnPStatus status;
  gchar* control_url;
  const gchar * service_type;
  char publicip[16];
} GaimUPnPControlInfo;

typedef struct {
  const gchar * service_type;
  gchar * full_url;
  char * buf;
  unsigned int buf_len;
  int sock;
} UPnPDiscoveryData;

static GaimUPnPControlInfo control_info = {
  GAIM_UPNP_STATUS_UNDISCOVERED, 
  NULL,
  NULL,
  "",
};


/**
 * This is the signature used for functions that act as a callback
 * to CURL.
 */
typedef size_t (*GaimUtilFetchUrlCallback)(void *url_data,
					   size_t size,
					   size_t nmemb,
					   gpointer user_data);



static gboolean
gaim_upnp_compare_device(const xmlnode* device, 
			 const gchar* deviceType) {
  xmlnode* deviceTypeNode = xmlnode_get_child(device, "deviceType");
  char * tmp;
  gboolean ret;
  
  if (deviceTypeNode == NULL) 
    return FALSE;	
  tmp = xmlnode_get_data(deviceTypeNode);
  ret = !g_ascii_strcasecmp(tmp, deviceType);
  g_free(tmp);  
  return ret;
}

static gboolean
gaim_upnp_compare_service(const xmlnode* service, 
			  const gchar* serviceType) {
  xmlnode * serviceTypeNode;
  char *tmp;
  gboolean ret;
  
  if (service == NULL) 
    return FALSE;	
  serviceTypeNode = xmlnode_get_child(service, "serviceType");
  if(serviceTypeNode == NULL) 
    return FALSE;
  tmp = xmlnode_get_data(serviceTypeNode);
  ret = !g_ascii_strcasecmp(tmp, serviceType);
  g_free(tmp); 
  return ret;
}

static gchar*
gaim_upnp_parse_description_response(const gchar* httpResponse, 
				     gsize len,
				     const gchar* httpURL,
				     const gchar* serviceType) {
  gchar *xmlRoot, *baseURL, *controlURL, *service;
  xmlnode *xmlRootNode, *serviceTypeNode, *controlURLNode, *baseURLNode;
  char *tmp;
  
  /* make sure we have a valid http response */
  if(g_strstr_len(httpResponse, len, HTTP_OK) == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): Failed In HTTP_OK\n");
    return NULL;
  }
  
  /* find the root of the xml document */
  if((xmlRoot = g_strstr_len(httpResponse, len, "<root")) == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): Failed finding root\n");
    return NULL;
  }
  
  /* create the xml root node */
  if((xmlRootNode = xmlnode_from_str(xmlRoot,
				     len - (xmlRoot - httpResponse))) == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): Could not parse xml root node\n");
    return NULL;
  }
  
  /* get the baseURL of the device */
  if((baseURLNode = xmlnode_get_child(xmlRootNode, "URLBase")) != NULL) {
    baseURL = xmlnode_get_data(baseURLNode);
  } else {
    baseURL = g_strdup(httpURL);
  }
  
  /* get the serviceType child that has the service type as its data */
  
  /* get urn:schemas-upnp-org:device:InternetGatewayDevice:1 and its devicelist */
  serviceTypeNode = xmlnode_get_child(xmlRootNode, "device");
  while(!gaim_upnp_compare_device(serviceTypeNode,
				  "urn:schemas-upnp-org:device:InternetGatewayDevice:1") &&
	serviceTypeNode != NULL) {
    serviceTypeNode = xmlnode_get_next_twin(serviceTypeNode);
  }
  if(serviceTypeNode == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): could not get serviceTypeNode 1\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  serviceTypeNode = xmlnode_get_child(serviceTypeNode, "deviceList");
  if(serviceTypeNode == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): could not get serviceTypeNode 2\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  
  /* get urn:schemas-upnp-org:device:WANDevice:1 and its devicelist */
  serviceTypeNode = xmlnode_get_child(serviceTypeNode, "device");
  while(!gaim_upnp_compare_device(serviceTypeNode,
				  "urn:schemas-upnp-org:device:WANDevice:1") &&
	serviceTypeNode != NULL) {
    serviceTypeNode = xmlnode_get_next_twin(serviceTypeNode);
  }
  if(serviceTypeNode == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): could not get serviceTypeNode 3\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  serviceTypeNode = xmlnode_get_child(serviceTypeNode, "deviceList");
  if(serviceTypeNode == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): could not get serviceTypeNode 4\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  
  /* get urn:schemas-upnp-org:device:WANConnectionDevice:1 and its servicelist */
  serviceTypeNode = xmlnode_get_child(serviceTypeNode, "device");
  while(serviceTypeNode && !gaim_upnp_compare_device(serviceTypeNode,
						     "urn:schemas-upnp-org:device:WANConnectionDevice:1")) {
    serviceTypeNode = xmlnode_get_next_twin(serviceTypeNode);
  }
  if(serviceTypeNode == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): could not get serviceTypeNode 5\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  serviceTypeNode = xmlnode_get_child(serviceTypeNode, "serviceList");
  if(serviceTypeNode == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): could not get serviceTypeNode 6\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  
  /* get the serviceType variable passed to this function */
  service = g_strdup_printf(SEARCH_REQUEST_DEVICE, serviceType);
  serviceTypeNode = xmlnode_get_child(serviceTypeNode, "service");
  while(!gaim_upnp_compare_service(serviceTypeNode, service) &&
	serviceTypeNode != NULL) {
    serviceTypeNode = xmlnode_get_next_twin(serviceTypeNode);
  }
  
  g_free(service);
  if(serviceTypeNode == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): could not get serviceTypeNode 7\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  
  /* get the controlURL of the service */
  if((controlURLNode = xmlnode_get_child(serviceTypeNode,
					 "controlURL")) == NULL) {
    gaim_debug_error("upnp",
		     "parse_description_response(): Could not find controlURL\n");
    g_free(baseURL);
    xmlnode_free(xmlRootNode);
    return NULL;
  }
  
  tmp = xmlnode_get_data(controlURLNode);
  if(baseURL && !gaim_str_has_prefix(tmp, "http://") &&
     !gaim_str_has_prefix(tmp, "HTTP://")) {
    controlURL = g_strdup_printf("%s%s", baseURL, tmp);
    g_free(tmp);
  } else{
    controlURL = tmp;
  }
  g_free(baseURL);
  xmlnode_free(xmlRootNode);
  
  return controlURL;
}

#define CURL_EASY_SETOPT(c, a, b) do { ret = curl_easy_setopt(c, a, b); if (ret != CURLE_OK) GE_LOG(NULL, GE_WARNING | GE_USER | GE_BULK, _("%s failed at %s:%d: `%s'\n"), "curl_easy_setopt", __FILE__, __LINE__, curl_easy_strerror(ret)); } while (0);

/**
 * Do the generic curl setup.
 */
static int setup_curl(const char * proxy,
		      CURL * curl) {
  int ret;

  ret = CURLE_OK;
  CURL_EASY_SETOPT(curl,
		   CURLOPT_FAILONERROR,
		   1);
  if (strlen(proxy) > 0)
    CURL_EASY_SETOPT(curl,
		     CURLOPT_PROXY,
		     proxy);
  CURL_EASY_SETOPT(curl,
		   CURLOPT_BUFFERSIZE,
		   1024); /* a bit more than one HELLO */
  CURL_EASY_SETOPT(curl,
		   CURLOPT_CONNECTTIMEOUT,
		   150L);
  /* NOTE: use of CONNECTTIMEOUT without also
     setting NOSIGNAL results in really weird
     crashes on my system! */
  CURL_EASY_SETOPT(curl,
		   CURLOPT_NOSIGNAL,
		   1);
  if (ret != CURLE_OK)
    return SYSERR;
  return OK;
}

static int
gaim_upnp_generate_action_message_and_send(const char * proxy,
					   const gchar* actionName,
					   const gchar* actionParams, 
					   GaimUtilFetchUrlCallback cb,
					   gpointer cb_data) {
  gchar * soapMessage;
  CURL * curl;	
  gchar * postfields;
  int ret;

  if (0 != curl_global_init(CURL_GLOBAL_WIN32)) 
    return SYSERR;
  /* set the soap message */
  soapMessage = g_strdup_printf(SOAP_ACTION, 
				actionName,
				control_info.service_type, 
				actionParams, 
				actionName);
  postfields = g_strdup_printf(HTTP_POST_SOAP_ACTION, 
			       control_info.service_type, 
			       actionName,
			       strlen(soapMessage),
			       soapMessage);
  g_free(soapMessage);
  curl = curl_easy_init();
  setup_curl(proxy, curl);
  ret = CURLE_OK;
  CURL_EASY_SETOPT(curl,
		   CURLOPT_URL,
		   control_info.control_url);
  CURL_EASY_SETOPT(curl,
		   CURLOPT_WRITEFUNCTION,
		   cb);
  CURL_EASY_SETOPT(curl,
		   CURLOPT_WRITEDATA,
		   cb_data);
  CURL_EASY_SETOPT(curl,
		   CURLOPT_POST,
		   1);
  CURL_EASY_SETOPT(curl,
		   CURLOPT_POSTFIELDS,
		   postfields);
  if (ret == CURLE_OK)
    ret = curl_easy_perform(curl);
  if (ret != CURLE_OK)
    GE_LOG(NULL,
	   GE_ERROR | GE_ADMIN | GE_DEVELOPER | GE_BULK,
	   _("%s failed at %s:%d: `%s'\n"),
	   "curl_easy_perform",
	   __FILE__,
	   __LINE__,
	   curl_easy_strerror(ret));
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  g_free(postfields);
  if (ret != CURLE_OK)
    return SYSERR;
  return OK;
}


static size_t
looked_up_public_ip_cb(void *url_data, 
		       size_t size,
		       size_t nmemb,		      
		       gpointer user_data) {
  UPnPDiscoveryData * dd = user_data;
  size_t len = size * nmemb;
  const gchar * temp;
  const gchar * temp2;

  if (len + dd->buf_len > 1024 * 1024 * 4)
    return len; /* refuse to process - too big! */
  GROW(dd->buf,
       dd->buf_len,
       dd->buf_len + len);
  memcpy(&dd->buf[dd->buf_len - len],
	 url_data,
	 len);
  if (dd->buf_len == 0)
    return len;
  /* extract the ip, or see if there is an error */
  if ((temp = g_strstr_len(dd->buf,
			   dd->buf_len,
			   "<NewExternalIPAddress")) == NULL) 
    return len;  
  if (!(temp = g_strstr_len(temp, 
			    dd->buf_len - (temp - dd->buf), ">"))) 
    return len;
  if (!(temp2 = g_strstr_len(temp, 
			     dd->buf_len - (temp - dd->buf), "<"))) 
    return len;
  memset(control_info.publicip, 
	 0,
	 sizeof(control_info.publicip));
  if (temp2 - temp >= sizeof(control_info.publicip))
    temp2 = temp + sizeof(control_info.publicip) - 1;
  memcpy(control_info.publicip, 
	 temp + 1,
	 temp2 - (temp + 1));
  GE_LOG(NULL,
	 GE_INFO | GE_USER | GE_BULK,
	 _("upnp: NAT Returned IP: %s\n"),
	 control_info.publicip);
  return len;
}

/**
 * Process downloaded bits of service description.
 */
static size_t
upnp_parse_description_cb(void * httpResponse,
			  size_t size,
			  size_t nmemb,
			  void * user_data) {
  UPnPDiscoveryData * dd = user_data;
  gsize len = size * nmemb;
  gchar * control_url = NULL;
  
  if (len + dd->buf_len > 1024 * 1024 * 4)
    return len; /* refuse to process - too big! */
  GROW(dd->buf,
       dd->buf_len,
       dd->buf_len + len);
  memcpy(&dd->buf[dd->buf_len - len],
	 httpResponse,
	 len);
  if (dd->buf_len > 0)
    control_url = gaim_upnp_parse_description_response(dd->buf,
						       dd->buf_len,
						       dd->full_url,
						       dd->service_type);
  control_info.status = control_url ? GAIM_UPNP_STATUS_DISCOVERED
    : GAIM_UPNP_STATUS_UNABLE_TO_DISCOVER;
  FREENONNULL(control_info.control_url);
  control_info.control_url = control_url;
  control_info.service_type = dd->service_type;
  return len;
}

static int
gaim_upnp_parse_description(char * proxy,
			    UPnPDiscoveryData * dd) {
  CURL * curl;	
  int ret;
  
  if (0 != curl_global_init(CURL_GLOBAL_WIN32)) 
    return SYSERR;
  curl = curl_easy_init();
  setup_curl(proxy, curl);
  ret = CURLE_OK;
  CURL_EASY_SETOPT(curl,
		   CURLOPT_URL,
		   dd->full_url);
  CURL_EASY_SETOPT(curl,
		   CURLOPT_WRITEFUNCTION,
		   &upnp_parse_description_cb);
  CURL_EASY_SETOPT(curl,
		   CURLOPT_WRITEDATA,
		   dd);
  ret = curl_easy_perform(curl);
  if (ret != CURLE_OK)
    GE_LOG(NULL,
	   GE_ERROR | GE_ADMIN | GE_DEVELOPER | GE_BULK,
	   _("%s failed at %s:%d: `%s'\n"),
	   "curl_easy_perform",
	   __FILE__,
	   __LINE__,
	   curl_easy_strerror(ret));
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  if (control_info.control_url == NULL)
    return SYSERR;
  return OK;
}

int
gaim_upnp_discover(struct GE_Context * ectx,
		   struct GC_Configuration * cfg) {
  char * proxy;
  struct hostent* hp;
  struct sockaddr_in server;
  int retry_count;
  gchar * sendMessage;
  gsize totalSize;
  gboolean sentSuccess;
  gchar buf[65536];
  int buf_len;
  const gchar * startDescURL;
  const gchar * endDescURL;
  int ret;
  UPnPDiscoveryData dd;

  memset(&dd,
	 0, 
	 sizeof(UPnPDiscoveryData));
  if (control_info.status == GAIM_UPNP_STATUS_DISCOVERING) 
    return NO;
  dd.sock = SOCKET(AF_INET, SOCK_DGRAM, 0);
  if (dd.sock == -1) 
    return SYSERR;
  hp = gethostbyname(HTTPMU_HOST_ADDRESS);
  if (hp == NULL) {
    CLOSE(dd.sock);
    return SYSERR; 
  } 
  memset(&server,
	 0, 
	 sizeof(struct sockaddr));
  server.sin_family = AF_INET;
  memcpy(&server.sin_addr,
	 hp->h_addr_list[0],
	 hp->h_length);
  server.sin_port = htons(HTTPMU_HOST_PORT);  
  control_info.status = GAIM_UPNP_STATUS_DISCOVERING;
  
  /* because we are sending over UDP, if there is a failure
     we should retry the send NUM_UDP_ATTEMPTS times. Also,
     try different requests for WANIPConnection and WANPPPConnection*/
  for (retry_count=0;retry_count<NUM_UDP_ATTEMPTS;retry_count++) {
    sentSuccess = FALSE;    
    if((retry_count % 2) == 0) 
      dd.service_type = WAN_IP_CONN_SERVICE;
    else 
      dd.service_type = WAN_PPP_CONN_SERVICE;    
    sendMessage = g_strdup_printf(SEARCH_REQUEST_STRING, 
				  dd.service_type);
    totalSize = strlen(sendMessage);
    do {
      if (SENDTO(dd.sock,
		 sendMessage, 
		 totalSize, 
		 0,
		 (struct sockaddr*) &server,
		 sizeof(struct sockaddr_in)) == totalSize) {
	sentSuccess = TRUE;
	break;
      }
    } while ( ((errno == EINTR) || (errno == EAGAIN)) &&
	      (GNUNET_SHUTDOWN_TEST() == NO));
    g_free(sendMessage);    
    if (sentSuccess) {
      gaim_timeout_add(DISCOVERY_TIMEOUT,
		       gaim_upnp_discover_timeout, 
		       &dd);
      break;
    }
  }
  if (sentSuccess == FALSE) {
    CLOSE(dd.sock);
    return SYSERR;
  }

  /* try to read response */
  do {
    buf_len = recv(dd.sock,
		   buf,
		   sizeof(buf) - 1, 
		   0);   
    if (buf_len > 0) {
      buf[buf_len] = '\0';
      break;
    } else if (errno != EINTR) {
      continue;
    }
  } while ( (errno == EINTR) &&
	    (GNUNET_SHUTDOWN_TEST() == NO) );
  CLOSE(dd.sock);

  /* parse the response, and see if it was a success */
  if (g_strstr_len(buf, buf_len, HTTP_OK) == NULL) 
    return SYSERR; 
  if ( (startDescURL = g_strstr_len(buf, buf_len, "http://")) == NULL) 
    return SYSERR;  
  
  endDescURL = g_strstr_len(startDescURL,
			    buf_len - (startDescURL - buf),
			    "\r");
  if (endDescURL == NULL) 
    endDescURL = g_strstr_len(startDescURL,
			      buf_len - (startDescURL - buf), "\n");
  if(endDescURL == NULL) 
    return SYSERR;  
  if (endDescURL == startDescURL) 
    return SYSERR;    
  dd.full_url = g_strndup(startDescURL,
			  endDescURL - startDescURL); 
  proxy = NULL; /* FIXME */
  ret = gaim_upnp_parse_description(proxy,
				    &dd);  
  g_free(dd.full_url);
  GROW(dd.buf,
       dd.buf_len,
       0);
  if (ret == OK) {
    ret = gaim_upnp_generate_action_message_and_send(proxy,
						     "GetExternalIPAddress", 
						     "",
						     looked_up_public_ip_cb, 
						     &dd);
    GROW(dd.buf,
	 dd.buf_len,
	 0);
  }
  return ret;
}

const gchar *
gaim_upnp_get_public_ip() {
  if (control_info.status == GAIM_UPNP_STATUS_DISCOVERED
      && control_info.publicip
      && strlen(control_info.publicip) > 0)
    return control_info.publicip;
  return NULL;
}

int
gaim_upnp_change_port_mapping(struct GE_Context * ectx,
			      struct GC_Configuration * cfg,
			      int do_add,
			      unsigned short portmap, 
			      const gchar* protocol) {
  const gchar * action_name;
  gchar * action_params;
  char * internal_ip;
  char * proxy;
  int ret;
    
  if (control_info.status != GAIM_UPNP_STATUS_DISCOVERED) 
    return NO;	
  if (do_add) {
    internal_ip = gaim_upnp_get_internal_ip(cfg,
					    ectx);
    if (internal_ip == NULL) {
      gaim_debug_error("upnp",
		       "gaim_upnp_set_port_mapping(): couldn't get local ip\n");
      return NO;
    }
    action_name = "AddPortMapping";
    action_params = g_strdup_printf(ADD_PORT_MAPPING_PARAMS,
				    portmap, 
				    protocol,
				    portmap,
				    internal_ip);
    FREE(internal_ip);
  } else {
    action_name = "DeletePortMapping";
    action_params = g_strdup_printf(DELETE_PORT_MAPPING_PARAMS,
				    portmap, 
				    protocol);
  }  
  proxy = NULL; /* FIXME! */
  ret = gaim_upnp_generate_action_message_and_send(proxy,
						   action_name,
						   action_params,
						   NULL,
						   NULL);
  
  g_free(action_params);
  return ret; 
}
