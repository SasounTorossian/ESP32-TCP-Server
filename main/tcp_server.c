/* tcp_server Example
Example written for Laprota by Sasoun Torossian

Combines example code from the the "esp-idf/examples/protocols/sockets/tcp_server/main/tcp_server.c" repository and "stations_AP.c"

Must first use mingw32.exe console to navigate to ~/station_AP folder,
and execute "make menuconfig".

From menuconfig, serial port that ESP32 is connected to must be chosen.
*/


/* PROBLEMS:
 *
 * Upon connecting phone send ESP32 127 + 31 bytes of data, establishing an immediate TCP/IP link. Prevents further connections unless
 * TCP/IP client app connects, then we restart the wifi connection.
 *
 * When ESP32 receives router ssid and password, unable to initialise STA_CONNECTION. Connection is constantly dropped.
 *
 */

#include <string.h>
#include <sys/param.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

static esp_err_t event_handler(void *ctx, system_event_t *event);
static void tcp_server_task(void *pvParameters);
void wifi_init_ap();
void wifi_init_sta(char* gateway_ssid, char* gateway_password);
char* getTagValue(char* a_tag_list, char* a_tag);


/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/

#define GATEWAY_SSID			"VM837614-2G" 	//Home wifi ssid
#define GATEWAY_PASS			"jwwdxwfq"		//Home wifi password
#define ESP_AP_SSID				"myESP"			//Ssid for ESP32 access point
#define ESP_AP_PASS				"password" 		//password for ESP32 access point
#define GATEWAY_MAX_RETRY		10 				//Number of times ESP32 will attempt to reconnect to router
#define ESP_AP_MAX_CONNECT		2				//Maximum stations that can connect to ESP32
#define PORT 					80

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

const int IPV4_GOTIP_BIT = BIT0;

static const char *TAG_STA = "wifi station";
static const char *TAG_AP = "wifi softAP";
static const char *TAG = "TCP/IP socket";

static int s_retry_num = 0;		//Initialise current retry to 0
static uint8_t MAC_array[6];	//Initialise MAC address
static char MAC_char[18];		//Display MAC characters

int client_socket;
int ip_protocol;
int socket_id;
int bind_err;
int listen_error;

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START: //Upon esp_wifi_start(), this event will arise
    	ESP_LOGI(TAG_STA, "SYSTEM_EVENT_STA_START");
    	ESP_ERROR_CHECK(esp_wifi_connect()); //Connect to detected Wifi network
        break;

    case SYSTEM_EVENT_STA_CONNECTED: //When esp_wifi_connect() is succesful
    	ESP_LOGI(TAG_STA, "SYSTEM_EVENT_STA_CONNECTED");
    	ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_MODE_STA, MAC_array));
        for (int i = 0; i < sizeof(MAC_array); ++i)
        {
           	sprintf(MAC_char,"%s%02x:",MAC_char,MAC_array[i]);
        }
        ESP_LOGI(TAG_STA, "got MAC: %s", MAC_char);
        xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
        break;

    case SYSTEM_EVENT_STA_GOT_IP: //When AP DHCP server provides an IP address to DHCP client (ESP32) this event will arise.
    	ESP_LOGI(TAG_STA, "SYSTEM_EVENT_STA_GOT_IP");
    	ESP_LOGI(TAG_STA, "got ip: %s",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
        break;

    case SYSTEM_EVENT_STA_DISCONNECTED: //If disconnection occurs, this event will arise
        {
        	ESP_LOGI(TAG_STA, "SYSTEM_EVENT_STA_DISCONNECTED");
            if (s_retry_num < GATEWAY_MAX_RETRY)
            {
                ESP_ERROR_CHECK(esp_wifi_connect());
                xEventGroupClearBits(wifi_event_group, IPV4_GOTIP_BIT);
                s_retry_num++;
                ESP_LOGI(TAG_STA,"retry to connect to the AP");
            }
            ESP_LOGI(TAG_STA,"connect to the AP fail\n");
            break;
        }

    case SYSTEM_EVENT_AP_STACONNECTED: //When new stations connects to AP (ESP32), display MAC address and AID
    	ESP_LOGI(TAG_STA, "SYSTEM_EVENT_AP_STACONNECTED");
    	ESP_LOGI(TAG_AP, "station:"MACSTR" join, AID= %d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;

    case SYSTEM_EVENT_AP_STADISCONNECTED: //When stations disconnect from AP (ESP32), display deisconnected stations' MAC and AID
    	ESP_LOGI(TAG_STA, "SYSTEM_EVENT_AP_STADISCONNECTED");
    	ESP_LOGI(TAG_AP, "station:"MACSTR"leave, AID= %d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
    	//On disconnet, close TCP socket client
    	if (client_socket != -1)
    	{
    		ESP_LOGE(TAG, "Shutting down socket and restarting...");
    		shutdown(client_socket, 0);
    		close(client_socket);
    	}
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* PROBLEMS
 * Set up AP first, then when router info is received, start APSTA mode.
 *
 * Causes issues with ESP32 connecting with router as STA
 *
 * ??Deactivate AP mode first, then activate APSTA mode??
 *
 * ??Reinitialise WiFi from scratch??
 *
 * ??Issues with ESP32 channels??
 *
 * ??Try hardcoding router ssid/password, then seeing result??
 *
 * ??Hardcoding station ID works?
 *
 * ??Why does passing variable not work??????????????????????
 * Need to brush up on char array manipulation techniques
 *
 */

void wifi_init_ap()
{
    wifi_event_group = xEventGroupCreate(); 					//Create listener thread

    tcpip_adapter_init(); 										//Initialise lwIP
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL) ); //Start event_handler loop

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); 	//Create instance of wifi_init_config_t cfg, and assign default values to all members
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); 					//Initialise instance of wifi_init_config_t

    wifi_config_t wifi_config_ap = { 						//Set configuration parameters for AP mode
        .ap = {
            .ssid = ESP_AP_SSID, 							//SSID of ESP32 AP
            .ssid_len = strlen(ESP_AP_SSID), 				//Length of SSID
            .password = ESP_AP_PASS, 						//ESP32 AP password
            .max_connection = ESP_AP_MAX_CONNECT, 			//Maximum allowed connections
            .authmode = WIFI_AUTH_WPA_WPA2_PSK 				//Authorization type. Secure WPA2_PSK protocol
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP) ); 						//Set Wifi mode as AP+Station
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap)); 		//Initialise AP configuration
    ESP_LOGI(TAG_AP, "wifi_init_ap finished SSID: %s, password: %s",
    		ESP_AP_SSID, ESP_AP_PASS);									//Print what credentials the ESP32 is broadcasting as an AP

    ESP_ERROR_CHECK(esp_wifi_start()); 										//Start the Wifi driver
}

void wifi_init_sta(char* gateway_ssid, char* gateway_password)
{
	/*
	char* ssid_copy = malloc(strlen(gateway_ssid) + 1);
	char* password_copy = malloc(strlen(gateway_password) + 1);

	strcpy(ssid_copy, gateway_ssid);
	strcpy(password_copy, gateway_password);
	*/

	wifi_config_t wifi_config_sta;
	strcpy((char*)wifi_config_sta.sta.ssid, gateway_ssid);
	strcpy((char*)wifi_config_sta.sta.password, gateway_password);

	/*
    wifi_config_t wifi_config_sta = { 						//Set configuration parameters for station mode
        .sta = {
            .ssid = ssid_copy,							//Home router SSID
            .password = password_copy, 						//Home router password
			.scan_method = WIFI_ALL_CHANNEL_SCAN 			//Scan mode to detect home router
        },
	};
	*/
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA) ); 					//Set Wifi mode as AP+Station
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_sta)); 	//Initialise Station configuration
    ESP_LOGI(TAG_STA, "wifi_init_sta finished SSID: %s, password: %s",
    		GATEWAY_SSID, GATEWAY_PASS); 								//Print which AP the ESP32 will be connecting to
    ESP_LOGI(TAG_STA, "wifi_init_sta finished provided SSID: %s, password: %s",
    		gateway_ssid, gateway_password);
    ESP_ERROR_CHECK(esp_wifi_start());
}


static void tcp_server_task(void *pvParameters)
{
    char rx_buffer[128];	// char array to store received data
    char addr_str[128];		// char array to store client IP
    int bytes_received;		// immediate bytes received
    int addr_family;		// Ipv4 address protocol variable

    while (1)
    {
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY); //Change hostname to network byte order
        destAddr.sin_family = AF_INET;		//Define address family as Ipv4
        destAddr.sin_port = htons(PORT); 	//Define PORT
        addr_family = AF_INET;				//Define address family as Ipv4
        ip_protocol = IPPROTO_TCP;			//Define protocol as TCP
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);

        /* Create TCP socket*/
        socket_id = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (socket_id < 0)
        {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

        /* Bind a socket to a specific IP + port */
        bind_err = bind(socket_id, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (bind_err != 0)
        {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket binded");

        /* Begin listening for clients on socket */
        listen_error = listen(socket_id, 3);
        if (listen_error != 0)
        {
            ESP_LOGE(TAG, "Error occured during listen: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket listening");

        while (1)
        {
        	struct sockaddr_in sourceAddr; // Large enough for IPv4
        	uint addrLen = sizeof(sourceAddr);
        	/* Accept connection to incoming client */
        	client_socket = accept(socket_id, (struct sockaddr *)&sourceAddr, &addrLen);
        	if (client_socket < 0)
        	{
        		ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
        		break;
        	}
        	ESP_LOGI(TAG, "Socket accepted");

			//Optionally set O_NONBLOCK
			//If O_NONBLOCK is set then recv() will return, otherwise it will stall until data is received or the connection is lost.
			//fcntl(client_socket,F_SETFL,O_NONBLOCK);

			// Clear rx_buffer, and fill with zero's
			bzero(rx_buffer, sizeof(rx_buffer));
        	vTaskDelay(500 / portTICK_PERIOD_MS);
			while(1)
			{
				ESP_LOGI(TAG, "Waiting for data");
				bytes_received = recv(client_socket, rx_buffer, sizeof(rx_buffer) - 1, 0);
				ESP_LOGI(TAG, "Received Data");

				// Error occured during receiving
				if (bytes_received < 0)
				{
					ESP_LOGI(TAG, "Waiting for data");
					vTaskDelay(100 / portTICK_PERIOD_MS);
				}
				// Connection closed
				else if (bytes_received == 0)
				{
					ESP_LOGI(TAG, "Connection closed");
					break;
				}
				// Data received
				else
				{
					// Get the sender's ip address as string
					if (sourceAddr.sin_family == PF_INET)
					{
						inet_ntoa_r(((struct sockaddr_in *)&sourceAddr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
					}

					rx_buffer[bytes_received] = 0; // Null-terminate whatever we received and treat like a string
					ESP_LOGI(TAG, "Received %d bytes from %s:", bytes_received, addr_str);
					ESP_LOGI(TAG, "%s", rx_buffer);

					char* ssid  = getTagValue(rx_buffer, "ssid");
					char* password  = getTagValue(rx_buffer, "password");

					if(ssid && password)
					{
						ESP_LOGI(TAG, "SSID: %s", ssid);
						ESP_LOGI(TAG, "PASSWORD: %s", password);
						wifi_init_sta(ssid, password);
					}

					// Clear rx_buffer, and fill with zero's
					bzero(rx_buffer, sizeof(rx_buffer));

				}
			}
        }
    }
    vTaskDelete(NULL);
}

char* getTagValue(char* a_tag_list, char* a_tag)
{
    /* 'strtok' modifies the string. */
    char* tag_list_copy = malloc(strlen(a_tag_list) + 1);
    char* result        = 0;
    char* s;

    strcpy(tag_list_copy, a_tag_list); //original to copy


    s = strtok(tag_list_copy, "&"); //Use delimiter "&"
    while (s)
    {
        char* equals_sign = strchr(s, '=');
        if (equals_sign)
        {
            *equals_sign = 0;
            //Use string compare to find required tag
            if (0 == strcmp(s, a_tag))
            {
                equals_sign++;
                result = malloc(strlen(equals_sign) + 1);
                strcpy(result, equals_sign);
            }
        }
        s = strtok(0, "&");
    }
    free(tag_list_copy);
    return result;
}

void app_main()
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    wifi_init_ap();
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}
