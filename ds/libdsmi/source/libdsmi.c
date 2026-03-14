#include <stdbool.h>
#include <nds.h>

#include <dswifi9.h>
#ifdef __BLOCKSDS__
#include <sys/ioctl.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "libdsmi.h"
#include "dserial.h"
#include "firmware_bin.h"
#include "osc_client.h"

#ifdef DSMI_SUPPORT_USB
#include "tusb.h"
#endif

#define PC_PORT		9000
#define DS_PORT		9001
#define DS_SENDER_PORT	9002

int sock, sockin;
struct sockaddr_in addr_out_from, addr_out_to, addr_in;

OSCbuf osc_buffer;
OSCbuf osc_recv_buff;

dsmi_type_t default_interface = -1;

#ifdef DSMI_SUPPORT_DSERIAL
bool dserial_enabled = false;
#endif
#ifdef DSMI_SUPPORT_WIFI
bool wifi_enabled = false;
#endif
#ifdef DSMI_SUPPORT_USB
bool usb_enabled = false;
#endif

extern void wifiValue32Handler(u32 value, void* data);
extern void arm9_synctoarm7();

// ------------ PRIVATE ------------ //

void dsmi_uart_recv(char * data, unsigned int size)
{
	// TODO
}

// ------------ SETUP ------------ //

// If a DSerial is inserted, this sets up the connection to the DSerial.
// Else, it connects to the default access point stored in Nintendo WFC
// memory (use an official game, e.g. mario kart to set this up)
// The initialized interface is set as the default interface.
//
// Returns 1 if connected, and 0 if failed.
int dsmi_connect(void)
{
#ifdef DSMI_SUPPORT_DSERIAL
	if(dsmi_connect_dserial()) {
		return 1;
	}
#endif
#ifdef DSMI_SUPPORT_USB
	if(dsmi_connect_usb()) {
		return 1;
	}
#endif
#ifdef DSMI_SUPPORT_WIFI
	if(dsmi_connect_wifi()) {
		return 1;
	}
#endif
	return 0;
}

#ifdef DSMI_SUPPORT_DSERIAL
// Using these you can force a wifi connection even if a DSerial is
// inserted or set up both connections for forwarding.
int dsmi_connect_dserial(void)
{
	if (!dserial_enabled) {
		if(!dseInit())
			return 0;

		int version = dseVersion();
		//if(version < 2) {
		//	printf("Version: DSerial1/2\n");
		//} else if(version == 2) {
		//	printf("Version: DSerial Edge\n");
		//}

		// Upload firmware if necessary
		if (!dseMatchFirmware((char*)firmware_bin, firmware_bin_size)) {
			dseUploadFirmware((char *) firmware_bin, firmware_bin_size);
		}

		dseBoot();

		swiDelay(9999); // Wait for the FW to boot
		if (dseStatus() != FIRMWARE)
			return 0;

		dseSetModes(ENABLE_CMOS);
		dseUartSetBaudrate(UART0, 31250); // MIDI baud rate
		dseUartSetReceiveHandler(UART0, dsmi_uart_recv);
	}

	default_interface = DSMI_SERIAL;
	dserial_enabled = true;

	return 1;
}
#endif

#ifdef DSMI_SUPPORT_WIFI
#ifdef __BLOCKSDS__
int dsmi_wifi_thread(void *arg)
{
    int counter = 0;
    while(wifi_enabled && default_interface == DSMI_WIFI)
    {
        cothread_yield_irq(IRQ_VBLANK);
        // Send a keepalive beacon every 3 seconds
        counter++;
        if (counter >= 60 * 3)
        {
            counter = 0;
            dsmi_write(0, 0, 0);
        }
    }
    return 0;
}
#else
void dsmi_timer_50ms(void) {
    Wifi_Timer(50);

    if(wifi_enabled && default_interface == DSMI_WIFI)
    {
        // Send a keepalive beacon every 3 seconds
        static u8 counter = 0;
        counter++;
        if(counter == 60)
        {
            counter = 0;
            dsmi_write(0, 0, 0);
        }
    }
}
#endif
#endif

#ifdef DSMI_SUPPORT_WIFI
// Modified version of dswifi's init function that uses a custom timer handler
// In addition to calling Wifi_Timer, new new handler also sends the DSMI keepalive
// beacon.
bool dsmi_wifi_init(void) {
#ifdef __BLOCKSDS__
    if (!Wifi_InitDefault(WFC_CONNECT | WIFI_ATTEMPT_DSI_MODE))
        return false;

    Wifi_EnableWifi();
    return true;
#else
    if (!Wifi_InitDefault(true))
        return false;

    irqSet(IRQ_TIMER3, dsmi_timer_50ms); // steal timer IRQ
    return true;
#endif
}

int dsmi_connect_wifi(void)
{
	if(!dsmi_wifi_init()) {
		return 0;
	}
	
	int i = Wifi_AssocStatus();
	if(i == ASSOCSTATUS_CANNOTCONNECT) {
		return 0;
	} else if(i == ASSOCSTATUS_ASSOCIATED) {
		sock = socket(AF_INET, SOCK_DGRAM, 0); // setup socket for DGRAM (UDP), returns with a socket handle
		sockin = socket(AF_INET, SOCK_DGRAM, 0);
		
		// Source
		addr_out_from.sin_family = AF_INET;
		addr_out_from.sin_port = htons(DS_SENDER_PORT);
		addr_out_from.sin_addr.s_addr = INADDR_ANY;
		
		// Destination
		addr_out_to.sin_family = AF_INET;
		addr_out_to.sin_port = htons(PC_PORT);
		
		struct in_addr gateway, snmask, dns1, dns2;
		Wifi_GetIPInfo(&gateway, &snmask, &dns1, &dns2);

		unsigned long my_ip = Wifi_GetIP(); // Set IP to broadcast IP
		unsigned long bcast_ip = my_ip | ~snmask.s_addr;
		
		addr_out_to.sin_addr.s_addr = bcast_ip;
		
		// Receiver
		addr_in.sin_family = AF_INET;
		addr_in.sin_port = htons(DS_PORT);
		addr_in.sin_addr.s_addr = INADDR_ANY;
		
		bind(sock, (struct sockaddr*)&addr_out_from, sizeof(addr_out_from));
		bind(sockin, (struct sockaddr*)&addr_in, sizeof(addr_in));
		
		u8 val = 1;
		ioctl(sockin, FIONBIO, (char*)&val);  // Enable non-blocking I/O
		
		default_interface = DSMI_WIFI;
		wifi_enabled = true;
#ifdef __BLOCKSDS__
		cothread_create(dsmi_wifi_thread, NULL, 256, COTHREAD_DETACHED);
#endif
		return 1;
	} else {
		return 0;
	}
}

void dsmi_disconnect_wifi(void)
{
	if (wifi_enabled) {
		close(sock);
		close(sockin);

		Wifi_DisconnectAP();
		Wifi_DisableWifi();

		default_interface = DSMI_NONE;
		wifi_enabled = false;
	}
}
#endif

#ifdef DSMI_SUPPORT_USB
int dsmi_connect_usb(void)
{
	tusb_rhport_init_t dev_init = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_AUTO
	};
	if (!tusb_init(BOARD_TUD_RHPORT, &dev_init))
		return 0;

	default_interface = DSMI_USB;
	usb_enabled = true;
	return 1;
}

void dsmi_disconnect_usb(void)
{
	if (usb_enabled) {
		tud_deinit(0);

		default_interface = DSMI_NONE;
		usb_enabled = false;
	}
}
#endif

void dsmi_disconnect(void)
{
#ifdef DSMI_SUPPORT_WIFI
	dsmi_disconnect_wifi();
#endif
#ifdef DSMI_SUPPORT_USB
	dsmi_disconnect_usb();
#endif
	default_interface = DSMI_NONE;
}

bool dsmi_task(void)
{
#ifdef DSMI_SUPPORT_USB
    if(usb_enabled) {
        tud_task();
        return true;
    }
#endif
    return false;
}

// ------------ WRITE ------------ //

// Send a MIDI message over the default interface, see MIDI spec for more details
void dsmi_write(u8 message,u8 data1, u8 data2)
{
#ifdef DSMI_SUPPORT_WIFI
	if(default_interface == DSMI_WIFI) {
		dsmi_write_wifi(message, data1, data2);
		return;
	}
#endif
#ifdef DSMI_SUPPORT_DSERIAL
	if(default_interface == DSMI_SERIAL) {
		dsmi_write_dserial(message, data1, data2);
		return;
	}
#endif
#ifdef DSMI_SUPPORT_USB
	if(default_interface == DSMI_USB) {
		dsmi_write_usb(message, data1, data2);
		return;
	}
#endif
}

#ifdef DSMI_SUPPORT_DSERIAL
// Force a MIDI message to be sent over DSerial
void dsmi_write_dserial(u8 message,u8 data1, u8 data2)
{
	u8 sendbuf[3] = {message, data1, data2};
	dseUartSendBuffer(UART0, (char*)sendbuf, 3, true);
}
#endif

#ifdef DSMI_SUPPORT_WIFI
// Force a MIDI message to be sent over Wifi
void dsmi_write_wifi(u8 message,u8 data1, u8 data2)
{
	char sendbuf[3] = {message, data1, data2};
	sendto(sock, &sendbuf, 3, 0, (struct sockaddr*)&addr_out_to, sizeof(addr_out_to));
}
#endif

#ifdef DSMI_SUPPORT_USB
// Force a MIDI message to be sent over USB
void dsmi_write_usb(u8 message,u8 data1, u8 data2)
{
	uint8_t msg[3] = {message, data1, data2};
	tud_midi_stream_write(0, msg, 3);
}
#endif

// ------------ OSC WRITE ------------ //

// Resets the OSC buffer and sets the destination open sound control address, returns 1 if ok, 0 if address string not valid
int dsmi_osc_new( char* addr){

  osc_init( &osc_buffer);
  return osc_writeaddr( &osc_buffer, addr);
  
}

// Adds arguments to the OSC packet
int dsmi_osc_addintarg( long arg){

  return osc_addintarg( &osc_buffer, arg);

}
int dsmi_osc_addstringarg( char* arg){

  return osc_addstringarg( &osc_buffer, arg);
}

int dsmi_osc_addfloatarg( float arg){

  return osc_addfloatarg( &osc_buffer, arg);

}

// Sends the OSC packet
int dsmi_osc_send(void){

  char* msg = osc_getPacket( &osc_buffer);
  int size = osc_getPacketSize( &osc_buffer);
  
  return sendto(sock, msg, size, 0, (struct sockaddr*)&addr_out_to, sizeof(addr_out_to));

}

// ------------ READ ------------ //

// Checks if a new message arrived at the default interface and returns it by
// filling the given pointers
//
// Returns 1, if a message was received, 0 if not
int dsmi_read(u8* message, u8* data1, u8* data2)
{
#ifdef DSMI_SUPPORT_WIFI
	if(default_interface == DSMI_WIFI)
		return dsmi_read_wifi(message, data1, data2);
#endif
#ifdef DSMI_SUPPORT_USB
	if(default_interface == DSMI_USB)
		return dsmi_read_usb(message, data1, data2);
#endif
	return 0;
}


// Force receiving over DSerial
// int dsmi_read_dserial(u8* message, u8* data1, u8* data2); // TODO

#ifdef DSMI_SUPPORT_WIFI
// Force receiving over Wifi
int dsmi_read_wifi(u8* message, u8* data1, u8* data2)
{
	char recbuf[3];
	socklen_t in_size;
	struct sockaddr_in in;

	int res = recvfrom(sockin, recbuf, 3, 0, (struct sockaddr*)&in, &in_size);
	
	if(res <= 0)
		return 0;
	
	*message = recbuf[0];
	*data1 = recbuf[1];
	*data2 = recbuf[2];
	
	return 1;
}
#endif

#ifdef DSMI_SUPPORT_USB
// Force receiving over USB
int dsmi_read_usb(u8* message, u8* data1, u8* data2)
{
	uint8_t recbuf[4];
	if (!tud_midi_available())
		return 0;

	tud_midi_packet_read(recbuf);

	*message = recbuf[1];
	*data1 = recbuf[2];
	*data2 = recbuf[3];

	return 1;
}
#endif

// ------------ OSC READ-------- //
int dsmi_osc_read(){
	socklen_t in_size;
	struct sockaddr_in in;

	int res = recvfrom(sockin, osc_recv_buff.buffer, OSC_MAX_SIZE, 0, (struct sockaddr*)&in, &in_size);
	
	if(res <= 0)
		return 0;

	return osc_decodePacket( &osc_recv_buff);
}
const char* dsmi_osc_getaddr(){

	return osc_getaddr( &osc_recv_buff);

}
int dsmi_osc_getnextarg( void* data, size_t* size, char* type ){

	return osc_getnextarg( &osc_recv_buff, data, size, type);

}

// ------------ MISC ------------ //

// Returns the default interface
dsmi_type_t dsmi_get_default_interface(void)
{
	return default_interface;
}
