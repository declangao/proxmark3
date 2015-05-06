//-----------------------------------------------------------------------------
// Ultralight Code (c) 2013,2014 Midnitesnake & Andy Davies of Pentura
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// High frequency MIFARE ULTRALIGHT (C) commands
//-----------------------------------------------------------------------------
#include "loclass/des.h"
#include "cmdhfmfu.h"
#include "cmdhfmf.h"
#include "cmdhf14a.h"
#include "mifare.h"
#include "util.h"
#include "../common/protocols.h"

#define MAX_UL_BLOCKS		0x0f
#define MAX_ULC_BLOCKS		0x2f
#define MAX_ULEV1a_BLOCKS	0x0b
#define MAX_ULEV1b_BLOCKS	0x20
#define MAX_NTAG_213		0x2c
#define MAX_NTAG_215		0x86
#define MAX_NTAG_216      0xe6

typedef enum TAGTYPE_UL {
	UNKNOWN     = 0x0000,
	UL          = 0x0001,
	UL_C        = 0x0002,
	UL_EV1_48   = 0x0004,
	UL_EV1_128  = 0x0008,
	NTAG        = 0x0010,
	NTAG_213    = 0x0020,
	NTAG_215    = 0x0040,
	NTAG_216    = 0x0080,
	MAGIC       = 0x0100,
	UL_MAGIC	= UL | MAGIC,
	UL_C_MAGIC	= UL_C | MAGIC,
	UL_ERROR	= 0xFFFF,
} TagTypeUL_t;

uint8_t default_3des_keys[7][16] = {
		{ 0x42,0x52,0x45,0x41,0x4b,0x4d,0x45,0x49,0x46,0x59,0x4f,0x55,0x43,0x41,0x4e,0x21 },// 3des std key
		{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },// all zeroes
		{ 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f },// 0x00-0x0F
		{ 0x49,0x45,0x4D,0x4B,0x41,0x45,0x52,0x42,0x21,0x4E,0x41,0x43,0x55,0x4F,0x59,0x46 },// NFC-key
		{ 0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01 },// all ones
		{ 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },// all FF
		{ 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF }	// 11 22 33
	};

uint8_t default_pwd_pack[3][4] = {
	{0xFF,0xFF,0xFF,0xFF}, // PACK 0x00,0x00 -- factory default
	{0x4A,0xF8,0x4B,0x19}, // PACK 0xE5,0xBE -- italian bus (sniffed)
	{0x05,0x22,0xE6,0xB4}  // PACK 0x80,0x80 -- Amiiboo (sniffed)
};	
	
static int CmdHelp(const char *Cmd);

char* getProductTypeStr( uint8_t id){

 	static char buf[20];
	char *retStr = buf;

	switch(id) {
	 case 3:
		sprintf(retStr, "0x%02X %s", id, "(Ultralight)");
		break;
	case 4:
		sprintf(retStr, "0x%02X %s", id, "(NTAG)");
		break;
	default:
		sprintf(retStr, "0x%02X %s", id, "(unknown)");
		break;
	}
	return buf;
}

/*
  The 7 MSBits (=n) code the storage size itself based on 2^n, 
  the LSBit is set to '0' if the size is exactly 2^n
  and set to '1' if the storage size is between 2^n and 2^(n+1). 
*/
char* getUlev1CardSizeStr( uint8_t fsize ){
 
 	static char buf[30];
	char *retStr = buf;

	uint8_t usize = 1 << ((fsize >>1) + 1);
	uint8_t lsize = 1 << (fsize >>1);
	
	// is  LSB set?
	if (  fsize & 1 )
		sprintf(retStr, "0x%02X (%u - %u bytes)",fsize, usize, lsize);
	else 
		sprintf(retStr, "0x%02X (%u bytes)", fsize, lsize);		
	return buf;
}

static void ul_switch_on_field(void) {
	UsbCommand c = {CMD_READER_ISO_14443a, {ISO14A_CONNECT | ISO14A_NO_DISCONNECT, 0, 0}};
	SendCommand(&c);
}

static void ul_switch_off_field(void) {
	UsbCommand c = {CMD_READER_ISO_14443a, {0, 0, 0}};
	SendCommand(&c);
}

static int ul_send_cmd_raw( uint8_t *cmd, uint8_t cmdlen, uint8_t *response, uint16_t responseLength ) {
	UsbCommand c = {CMD_READER_ISO_14443a, {ISO14A_RAW | ISO14A_NO_DISCONNECT | ISO14A_APPEND_CRC, cmdlen, 0}};
	memcpy(c.d.asBytes, cmd, cmdlen);
	SendCommand(&c);
	UsbCommand resp;
	if ( !WaitForResponseTimeout(CMD_ACK, &resp, 1500) ) return -1;
	if ( !resp.arg[0] && responseLength) return -1;
	
	uint16_t resplen = (resp.arg[0] < responseLength) ? resp.arg[0] : responseLength;
	memcpy(response, resp.d.asBytes, resplen);
	return resplen;
}
/*
static int ul_send_cmd_raw_crc( uint8_t *cmd, uint8_t cmdlen, uint8_t *response, uint16_t responseLength, bool append_crc ) {
	
	UsbCommand c = {CMD_READER_ISO_14443a, {ISO14A_RAW | ISO14A_NO_DISCONNECT , cmdlen, 0}};
	if (append_crc)
		c.arg[0] |= ISO14A_APPEND_CRC;
	
	memcpy(c.d.asBytes, cmd, cmdlen);	
	SendCommand(&c);
	UsbCommand resp;
	if (!WaitForResponseTimeout(CMD_ACK, &resp, 1500)) return -1;

	uint16_t resplen = (resp.arg[0] < responseLength) ? resp.arg[0] : responseLength;
	if (resp.arg[0] > 0) {
		memcpy(response, resp.d.asBytes, resplen );
	return resplen;
	} else return -1;
}
*/
static int ul_select( iso14a_card_select_t *card ){
	
	ul_switch_on_field();

	UsbCommand resp;
	if (!WaitForResponseTimeout(CMD_ACK, &resp, 1500)) return -1;
	if (resp.arg[0] < 0) return -1;
	
	memcpy(card, resp.d.asBytes, sizeof(iso14a_card_select_t));
	
	return resp.arg[0];
}

// This read command will at least return 16bytes.
static int ul_read( uint8_t page, uint8_t *response, uint16_t responseLength ){
	
	uint8_t cmd[] = {ISO14443A_CMD_READBLOCK, page};
	int len = ul_send_cmd_raw(cmd, sizeof(cmd), response, responseLength);
	if ( len == -1 )
		ul_switch_off_field();
	return len;
}

static int ul_comp_write( uint8_t page, uint8_t *data, uint8_t datalen ){

	uint8_t cmd[18];
	memset(cmd, 0x00, sizeof(cmd));
	datalen = ( datalen > 16) ? 16 : datalen;

	cmd[0] = ISO14443A_CMD_WRITEBLOCK;
	cmd[1] = page;
	memcpy(cmd+2, data, datalen);

	uint8_t response[1] = {0xff};
	int len = ul_send_cmd_raw(cmd, 2+datalen, response, sizeof(response));
	if ( len == -1 )
		ul_switch_off_field();
	// ACK
	if ( response[0] == 0x0a ) return 0;
	// NACK
	return -1;	
}

static int ulc_requestAuthentication( uint8_t blockNo, uint8_t *nonce, uint16_t nonceLength ){
	
	uint8_t cmd[] = {MIFARE_ULC_AUTH_1, blockNo};
	int len = ul_send_cmd_raw(cmd, sizeof(cmd), nonce, nonceLength);
	if ( len == -1 ) 
		ul_switch_off_field();
	return len;
}

static int ulev1_requestAuthentication( uint8_t *pwd, uint8_t *pack, uint16_t packLength ){
	
	uint8_t cmd[] = {MIFARE_ULEV1_AUTH, pwd[0], pwd[1], pwd[2], pwd[3]};
	int len = ul_send_cmd_raw(cmd, sizeof(cmd), pack, packLength);
	if ( len == -1)
		ul_switch_off_field();
	return len;
}

static int ulev1_getVersion( uint8_t *response, uint16_t responseLength ){
	
	uint8_t cmd[] = {MIFARE_ULEV1_VERSION};	
	int len = ul_send_cmd_raw(cmd, sizeof(cmd), response, responseLength);
	if ( len == -1 )
		ul_switch_off_field();
	return len;
}

// static int ulev1_fastRead( uint8_t startblock, uint8_t endblock, uint8_t *response ){
	
	// uint8_t cmd[] = {MIFARE_ULEV1_FASTREAD, startblock, endblock};
	
	// if ( !ul_send_cmd_raw(cmd, sizeof(cmd), response)){
		// ul_switch_off_field();
		// return -1;
	// }
	// return 0;
// }

static int ulev1_readCounter( uint8_t counter, uint8_t *response, uint16_t responseLength ){

	uint8_t cmd[] = {MIFARE_ULEV1_READ_CNT, counter};
	int len = ul_send_cmd_raw(cmd, sizeof(cmd), response, responseLength);
	if (len == -1)
		ul_switch_off_field();
	return len;
}

static int ulev1_readSignature( uint8_t *response, uint16_t responseLength ){

	uint8_t cmd[] = {MIFARE_ULEV1_READSIG, 0x00};
	int len = ul_send_cmd_raw(cmd, sizeof(cmd), response, responseLength);
	if (len == -1)
		ul_switch_off_field();
	return len;
}

static int ul_print_default( uint8_t *data){
	
	uint8_t uid[7];

	uid[0] = data[0];
	uid[1] = data[1];
	uid[2] = data[2];
	uid[3] = data[4];
	uid[4] = data[5];
	uid[5] = data[6];
	uid[6] = data[7];

	PrintAndLog("       UID : %s ", sprint_hex(uid, 7));
	PrintAndLog("    UID[0] : (Manufacturer Byte) = %02x, Manufacturer: %s",  uid[0], getTagInfo(uid[0]) );
	
	// BBC
	// CT (cascade tag byte) 0x88 xor SN0 xor SN1 xor SN2 
	int crc0 = 0x88 ^ data[0] ^ data[1] ^data[2];
	if ( data[3] == crc0 )
		PrintAndLog("      BCC0 : 0x%02X - Ok", data[3]);
	else
		PrintAndLog("      BCC0 : 0x%02X - crc should be %02x", data[3], crc0);
		
	int crc1 = data[4] ^ data[5] ^ data[6] ^data[7];
	if ( data[8] == crc1 )
		PrintAndLog("      BCC1 : 0x%02X - Ok", data[8]);
	else
		PrintAndLog("      BCC1 : 0x%02X - crc should be 0x%02X", data[8], crc1 );
	
	PrintAndLog("  Internal : 0x%02X - %s default", data[9], (data[9]==0x48)?"":"not" );
	PrintAndLog("      Lock : %s - %s", sprint_hex(data+10, 2),printBits( 2, data+10) );
	PrintAndLog("OneTimePad : %s ", sprint_hex(data + 12, 4));
	PrintAndLog("");
	return 0;
}

static int ntag_print_CC(uint8_t *data) {
	if(data[0] != 0xe1) {
		PrintAndLog("no NDEF message");
		return -1;		// no NDEF message
	}

	PrintAndLog("Capability Container: %s", sprint_hex(data,4) );
	PrintAndLog("  %02X: NDEF Magic Number", data[0]); 
	PrintAndLog("  %02X: version %d.%d supported by tag", data[1], (data[1] & 0xF0) >> 4, data[1] & 0x0f);
	PrintAndLog("  %02X: Physical Memory Size: %d bytes", data[2], (data[2] + 1) * 8);
	if ( data[2] == 0x12 )
		PrintAndLog("  %02X: NDEF Memory Size: &d bytes", data[2], 144);
	else if ( data[2] == 0x3e )
		PrintAndLog("  %02X: NDEF Memory Size: &d bytes", data[2], 496);
	else if ( data[2] == 0x6d )
		PrintAndLog("  %02X: NDEF Memory Size: &d bytes", data[2], 872);
	
	PrintAndLog("  %02X: %s / %s", data[3], 
				(data[3] & 0xF0) ? "(RFU)" : "Read access granted without any security", 
				(data[3] & 0x0F)==0 ? "Write access granted without any security" : (data[3] & 0x0F)==0x0F ? "No write access granted at all" : "(RFU)");
	return 0;				
}

static int ulev1_print_version(uint8_t *data){		
	PrintAndLog("\n--- UL-EV1 / NTAG Version");
	PrintAndLog("       Raw bytes : %s", sprint_hex(data, 8) );
	PrintAndLog("       Vendor ID : 0x%02X, Manufacturer: %s", data[1], getTagInfo(data[1]));
	PrintAndLog("    Product type : %s"		, getProductTypeStr(data[2]));
	PrintAndLog(" Product subtype : 0x%02X %s"	, data[3], (data[3]==1) ?"17 pF":"50pF");
	PrintAndLog("   Major version : 0x%02X"	, data[4]);
	PrintAndLog("   Minor version : 0x%02X"	, data[5]);
	PrintAndLog("            Size : %s", getUlev1CardSizeStr(data[6]));
	PrintAndLog("   Protocol type : 0x%02X"	, data[7]);
	return 0;
}

static int ul_print_type(uint16_t tagtype){
	if ( tagtype & UL )	
		PrintAndLog("      TYPE : MIFARE Ultralight (MF0ICU1) %s", (tagtype & MAGIC)?"<magic>":"");
	else if ( tagtype & UL_C)
		PrintAndLog("      TYPE : MIFARE Ultralight C (MF0ULC) %s [%x]", (tagtype & MAGIC)?"<magic>":"", tagtype );
	else if ( tagtype & UL_EV1_48)
		PrintAndLog("      TYPE : MIFARE Ultralight EV1 48bytes (MF0UL1101)"); 
	else if ( tagtype & UL_EV1_128)	
		PrintAndLog("      TYPE : MIFARE Ultralight EV1 128bytes (MF0UL2101)");
	else if ( tagtype & NTAG_213 )
		PrintAndLog("      TYPE : MIFARE NTAG 213 144bytes (NT2H1311G0DU)");
	else if ( tagtype & NTAG_215 )
		PrintAndLog("      TYPE : MIFARE NTAG 215 504bytes (NT2H1511G0DU)");
	else if ( tagtype & NTAG_216 )
		PrintAndLog("      TYPE : MIFARE NTAG 216 888bytes (NT2H1611G0DU)");
	else
		PrintAndLog("      TYPE : Unknown %04x",tagtype);	
	return 0;
}

static int ulc_print_3deskey( uint8_t *data){			
	PrintAndLog("         deskey1 [44/0x2C]: %s", sprint_hex(data   ,4));
	PrintAndLog("         deskey1 [45/0x2D]: %s", sprint_hex(data+4 ,4));			
	PrintAndLog("         deskey2 [46/0x2E]: %s", sprint_hex(data+8 ,4));
	PrintAndLog("         deskey2 [47/0x2F]: %s", sprint_hex(data+12,4));
	PrintAndLog(" 3des key : %s", sprint_hex(SwapEndian64(data, 16), 16));
	return 0;
}

static int ulc_print_configuration( uint8_t *data){
		
	PrintAndLog("--- UL-C Configuration");
	PrintAndLog(" Higher Lockbits [40/0x28]: %s %s", sprint_hex(data, 4), printBits(2, data));
	PrintAndLog("         Counter [41/0x29]: %s %s", sprint_hex(data+4, 4), printBits(2, data+4));

	bool validAuth = (data[8] >= 0x03 && data[8] <= 0x30);
	if ( validAuth )
		PrintAndLog("           Auth0 [42/0x2A]: %s - Pages above %d needs authentication", sprint_hex(data+8, 4), data[8] );
	else{
		if ( data[8] == 0){
			PrintAndLog("           Auth0 [42/0x2A]: %s - default", sprint_hex(data+8, 4) );
		} else {
			PrintAndLog("           Auth0 [42/0x2A]: %s - auth byte is out-of-range", sprint_hex(data+8, 4) );
		}
	}
	PrintAndLog("           Auth1 [43/0x2B]: %s - %s",
			sprint_hex(data+12, 4),
			(data[12] & 1) ? "write access restricted": "read and write access restricted"
			);
	return 0;
}

static int ulev1_print_configuration( uint8_t *data){

	PrintAndLog("\n--- UL-EV1 Configuration");	

	bool strg_mod_en = (data[0] & 2);
	uint8_t authlim = (data[4] & 0x07);
	bool cfglck = (data[4] & 0x40);
	bool prot = (data[4] & 0x80);
	uint8_t vctid = data[5];
	
	PrintAndLog(" cfg0 [16/0x10]: %s", sprint_hex(data, 4));
	PrintAndLog("                    - pages above %d needs authentication",data[3]);
	PrintAndLog("                    - strong modulation mode %s", (strg_mod_en) ? "enabled":"disabled");
	PrintAndLog(" cfg1 [17/0x11]: %s", sprint_hex(data+4, 4) );
	if ( authlim == 0)
		PrintAndLog("                    - Max number of password attempts is unlimited");
	else
		PrintAndLog("                    - Max number of password attempts is %d", authlim);
	PrintAndLog("                    - user configuration %s", cfglck ? "permanently locked":"writeable");
	PrintAndLog("                    - %s access is protected with password", prot ? "read and write":"write");
	PrintAndLog("               0x%02X - Virtual Card Type Identifier is %s default", vctid, (vctid==0x05)? "":"not");
	PrintAndLog(" PWD  [18/0x12]: %s", sprint_hex(data+8, 4));
	PrintAndLog(" PACK [19/0x13]: %s", sprint_hex(data+12, 4));
	return 0;
}

static int ulev1_print_counters(){
	PrintAndLog("--- UL-EV1 Counters");
	uint8_t counter[3] = {0,0,0};
	for ( uint8_t i = 0; i<3; ++i) {
		ulev1_readCounter(i,counter, sizeof(counter) );
		PrintAndLog("     [0x%0d] : %s", i, sprint_hex(counter,3));
	}
	return 0;
}

static int ulev1_print_signature( uint8_t *data, uint8_t len){
	PrintAndLog("\n--- UL-EV1 Signature");	
	PrintAndLog("IC signature public key name  : NXP NTAG21x 2013");
	PrintAndLog("IC signature public key value : 04494e1a386d3d3cfe3dc10e5de68a499b1c202db5b132393e89ed19fe5be8bc61");
	PrintAndLog("    Elliptic curve parameters : secp128r1");
	PrintAndLog("            Tag ECC Signature : %s", sprint_hex(data, len));
	//to do:  verify if signature is valid
	//PrintAndLog("IC signature status: %s valid", (iseccvalid() )?"":"not");
	return 0;
}

static int ulc_magic_test(){
	// Magic Ultralight test
		// Magic UL-C, by observation,
	// 1) it seems to have a static nonce response to 0x1A command.
	// 2) the deskey bytes is not-zero:d out on as datasheet states.
	// 3) UID - changeable, not only, but pages 0-1-2-3.
	// 4) use the ul_magic_test !  magic tags answers specially!
	int returnValue = UL_ERROR;
	iso14a_card_select_t card;
	uint8_t nonce1[11] = {0x00};
	uint8_t nonce2[11] = {0x00};
	int status = ul_select(&card);
	if ( status < 1 ){
		PrintAndLog("Error: couldn't select ulc_magic_test");
		ul_switch_off_field();
		return UL_ERROR;
	}
	status = ulc_requestAuthentication(0, nonce1, sizeof(nonce1));
	if ( status > 0 ) {
		status = ulc_requestAuthentication(0, nonce2, sizeof(nonce2));
		returnValue =  ( !memcmp(nonce1, nonce2, 11) ) ? UL_C_MAGIC : UL_C;
	} else {
		returnValue = UL;
	}	
	ul_switch_off_field();
	return returnValue;
}

static int ul_magic_test(){
	
	// Magic Ultralight test
	// 1) It takes present UID, and tries to write it back. OBSOLETE
	// 2) make a wrong length write to page0, and see if tag answers with ACK/NACK:
	iso14a_card_select_t card;
	int status = ul_select(&card);
	if ( status < 1 ){
		PrintAndLog("Error: couldn't select ul_magic_test");
		ul_switch_off_field();
		return UL_ERROR;
	}
	status = ul_comp_write(0, NULL, 0);
	ul_switch_off_field();
	if ( status == 0) 
		return UL_MAGIC;
	return UL;
}

uint16_t GetHF14AMfU_Type(void){

	TagTypeUL_t tagtype = UNKNOWN;
	iso14a_card_select_t card;
	uint8_t version[10] = {0x00};
	int status = 0;
	int len;

	status = ul_select(&card);
	if ( status < 1 ){
		PrintAndLog("Error: couldn't select");
		ul_switch_off_field();
		return UL_ERROR;
	}
	// Ultralight - ATQA / SAK 
	if ( card.atqa[1] != 0x00 || card.atqa[0] != 0x44 || card.sak != 0x00 ) {
		PrintAndLog("Tag is not UL or NTAG. [ATQA: %02X %02x SAK: %02X]", card.atqa[1],card.atqa[0],card.sak);
		ul_switch_off_field();
		return UL_ERROR;
	}
	
	len  = ulev1_getVersion(version, sizeof(version));
	ul_switch_off_field();
	
	switch (len) {
		case 0x0A: {
		
			if ( version[2] == 0x03 && version[6] == 0x0B ) 
				tagtype = UL_EV1_48;
			else if ( version[2] == 0x03 && version[6] != 0x0B ) 
				tagtype = UL_EV1_128;			
			else if ( version[2] == 0x04 && version[6] == 0x0F ) 
				tagtype = NTAG_213;
			else if ( version[2] == 0x04 && version[6] != 0x11 ) 
				tagtype = NTAG_215;	
			else if ( version[2] == 0x04 && version[6] == 0x13 ) 
				tagtype = NTAG_216;
			else if ( version[2] == 0x04 ) 
				tagtype = NTAG;
			
			break;
			}
		case 0x01: tagtype = UL_C; break;
		case 0x00: tagtype = UL; break;
		case -1:   tagtype = (UL | UL_C); break;
		default:   tagtype = UNKNOWN; break;
	}

	if ((tagtype & ( UL_C | UL ))) tagtype = ulc_magic_test();
	if ( (tagtype & UL) ) tagtype = ul_magic_test();

	return tagtype;
}	

int CmdHF14AMfUInfo(const char *Cmd){

	uint8_t authlim = 0xff;
	uint8_t data[16] = {0x00};
	iso14a_card_select_t card;
	uint8_t *key;
	int status;	

	TagTypeUL_t tagtype = GetHF14AMfU_Type();
	if (tagtype == UL_ERROR) return -1;

	PrintAndLog("\n--- Tag Information ---------");
	PrintAndLog("-------------------------------------------------------------");
	ul_print_type(tagtype);
	
	status = ul_select(&card);
	if ( status < 1 ){
		PrintAndLog("Error: couldn't select");
		ul_switch_off_field();
		return status;
	}
	
	// read pages 0,1,2,4 (should read 4pages)
	status = ul_read(0, data, sizeof(data));
	if ( status == -1 ){
		PrintAndLog("Error: tag didn't answer to READ A");
		ul_switch_off_field();
		return status;
	}
	
	ul_print_default(data);

	if ((tagtype & UL_C)){
	
		// lookup lockbits before?
		// read pages 0x28, 0x29, 0x2A, 0x2B
		// tag may be locked
		uint8_t ulc_conf[16] = {0x00};
		status = ul_read(0x28, ulc_conf, sizeof(ulc_conf));
		if ( status == -1 ){
			PrintAndLog("Error: tag didn't answer to READ - possibly locked");
		} else {
			ulc_print_configuration(ulc_conf);
		}

		if ((tagtype & MAGIC)){

			uint8_t ulc_deskey[16] = {0x00};
			status = ul_read(0x2C, ulc_deskey, sizeof(ulc_deskey));
			if ( status == -1 ){
				PrintAndLog("Error: tag didn't answer to READ B");
				ul_switch_off_field();
				return status;
			}
			
			ulc_print_3deskey(ulc_deskey);

		}
		else {
			PrintAndLog("Trying some default 3des keys");
			ul_switch_off_field();
			for (uint8_t i = 0; i < 7; ++i ){
				key = default_3des_keys[i];
				if (try3DesAuthentication(key) == 1){
					PrintAndLog("Found default 3des key: %s", sprint_hex(key,16));
					return 0;
				}
			}
		}
	}
	
	if ((tagtype & (UL_EV1_48 | UL_EV1_128))) {
			
		uint8_t startconfigblock = (tagtype & UL_EV1_48) ? 0x10 : 0x25;
		uint8_t ulev1_conf[16] = {0x00};
		status = ul_read(startconfigblock, ulev1_conf, sizeof(ulev1_conf));
		if ( status == -1 ){
			PrintAndLog("Error: tag didn't answer to READ C");
			ul_switch_off_field();
			return status;
		}
		// save AUTHENTICATION LIMITS for later:
		authlim = (ulev1_conf[4] & 0x07);
		
		ulev1_print_configuration(ulev1_conf);
		
		uint8_t ulev1_signature[32] = {0x00};
		status = ulev1_readSignature( ulev1_signature, sizeof(ulev1_signature));
		if ( status == -1 ){
			PrintAndLog("Error: tag didn't answer to READ SIGNATURE");
			ul_switch_off_field();
			return status;
		}		
		ulev1_print_signature( ulev1_signature, sizeof(ulev1_signature));
		
		ulev1_print_counters();
	}
	
	if ((tagtype & (UL_EV1_48 | UL_EV1_128 | NTAG_213 | NTAG_215 | NTAG_216))) {

		uint8_t version[10] = {0x00};
		status  = ulev1_getVersion(version, sizeof(version));
		if ( status == -1 ){
			PrintAndLog("Error: tag didn't answer to GETVERSION");
			ul_switch_off_field();
			return status;
		}
		ulev1_print_version(version);
		
		// AUTHLIMIT, (number of failed authentications)
		// 0 = limitless.
		// 1-7 = ...  should we even try then?
		if ( authlim == 0 ){
			PrintAndLog("\n--- Known EV1/NTAG passwords.");
		
			uint8_t pack[4] = {0,0,0,0};
			
			for (uint8_t i = 0; i < 3; ++i ){
				key = default_pwd_pack[i];
				if ( ulev1_requestAuthentication(key, pack, sizeof(pack)) > -1 ){
					PrintAndLog("Found a default password: %s || Pack: %02X %02X",sprint_hex(key, 4), pack[0], pack[1]);
				}
			}
			ul_switch_off_field();
		}
	}
	
	if ((tagtype & (NTAG_213 | NTAG_215 | NTAG_216))){
		
		PrintAndLog("\n--- NTAG NDEF Message");
		uint8_t cc[16] = {0x00};
		status = ul_read(2, cc, sizeof(cc));
		if ( status == -1 ){
			PrintAndLog("Error: tag didn't answer to READ D");
			ul_switch_off_field();
			return status;
		}
		ntag_print_CC(cc);	
	}
	
	ul_switch_off_field();
	return 0;
}

//
//  Mifare Ultralight Write Single Block
//
int CmdHF14AMfUWrBl(const char *Cmd){
	uint8_t blockNo    = -1;
	bool chinese_card  = FALSE;
	uint8_t bldata[16] = {0x00};
	UsbCommand resp;

	char cmdp = param_getchar(Cmd, 0);
	if (strlen(Cmd) < 3 || cmdp == 'h' || cmdp == 'H') {
		PrintAndLog("Usage:  hf mfu wrbl <block number> <block data (8 hex symbols)> [w]");
		PrintAndLog("       [block number]");
		PrintAndLog("       [block data] - (8 hex symbols)");
		PrintAndLog("       [w] - Chinese magic ultralight tag");
		PrintAndLog("");
		PrintAndLog("        sample: hf mfu wrbl 0 01020304");
		PrintAndLog("");		
		return 0;
	}       
	
	blockNo = param_get8(Cmd, 0);

	if (blockNo > MAX_UL_BLOCKS){
		PrintAndLog("Error: Maximum number of blocks is 15 for Ultralight Cards!");
		return 1;
	}
	
	if (param_gethex(Cmd, 1, bldata, 8)) {
		PrintAndLog("Block data must include 8 HEX symbols");
		return 1;
	}
	
	if (strchr(Cmd,'w') != 0  || strchr(Cmd,'W') != 0 ) {
		chinese_card = TRUE; 
	}
	
	if ( blockNo <= 3) {
		if (!chinese_card){
			PrintAndLog("Access Denied");
		} else {
			PrintAndLog("--specialblock no:%02x", blockNo);
			PrintAndLog("--data: %s", sprint_hex(bldata, 4));
			UsbCommand d = {CMD_MIFAREU_WRITEBL, {blockNo}};
			memcpy(d.d.asBytes,bldata, 4);
			SendCommand(&d);
			if (WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
				uint8_t isOK  = resp.arg[0] & 0xff;
				PrintAndLog("isOk:%02x", isOK);
			} else {
				PrintAndLog("Command execute timeout");
			}  
		}
	} else {
		PrintAndLog("--block no:%02x", blockNo);
		PrintAndLog("--data: %s", sprint_hex(bldata, 4));        	
		UsbCommand e = {CMD_MIFAREU_WRITEBL, {blockNo}};
		memcpy(e.d.asBytes,bldata, 4);
		SendCommand(&e);
		if (WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
			uint8_t isOK  = resp.arg[0] & 0xff;
			PrintAndLog("isOk:%02x", isOK);
		} else {
			PrintAndLog("Command execute timeout");
		}
	}
	return 0;
}

//
//  Mifare Ultralight Read Single Block
//
int CmdHF14AMfURdBl(const char *Cmd){

  	UsbCommand resp;
	uint8_t blockNo = -1;	
	char cmdp = param_getchar(Cmd, 0);
	
	if (strlen(Cmd) < 1 || cmdp == 'h' || cmdp == 'H') {	
		PrintAndLog("Usage:  hf mfu rdbl <block number>");
		PrintAndLog("        sample: hfu mfu rdbl 0");
		return 0;
	}       
		
	blockNo = param_get8(Cmd, 0);

	if (blockNo > MAX_UL_BLOCKS){
	   PrintAndLog("Error: Maximum number of blocks is 15 for Ultralight");
	   return 1;
	}

	UsbCommand c = {CMD_MIFAREU_READBL, {blockNo}};
	SendCommand(&c);


	if (WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
		uint8_t isOK = resp.arg[0] & 0xff;
		if (isOK) {
			uint8_t *data = resp.d.asBytes;
			PrintAndLog("Block: %0d (0x%02X) [ %s]", (int)blockNo, blockNo, sprint_hex(data, 4));
		}
		else {
			PrintAndLog("Failed reading block: (%02x)", isOK);
		}
	} else {
		PrintAndLog("Command execute time-out");
	}
	
	return 0;
}

int usage_hf_mfu_dump(void)
{
	PrintAndLog("Reads all pages from Ultralight, Ultralight-C, Ultralight EV1");
	PrintAndLog("and saves binary dump into the file `filename.bin` or `cardUID.bin`");
	PrintAndLog("It autodetects card type.\n");	
	PrintAndLog("Usage:  hf mfu dump k <key> n <filename w/o .bin>");
	PrintAndLog("  Options : ");
	PrintAndLog("  k <key> : Enter key for authentication");
	PrintAndLog("  n <FN > : Enter filename w/o .bin to save the dump as");	
	PrintAndLog("        s : Swap entered key's endianness for auth");
	PrintAndLog("");
	PrintAndLog("   sample : hf mfu dump");
	PrintAndLog("          : hf mfu dump n myfile");
	return 0;
}
//
//  Mifare Ultralight / Ultralight-C / Ultralight-EV1
//  Read and Dump Card Contents,  using auto detection of tag size.
//
//  TODO: take a password to read UL-C / UL-EV1 tags.
int CmdHF14AMfUDump(const char *Cmd){

	FILE *fout;
	char filename[FILE_PATH_SIZE] = {0x00};
	char * fnameptr = filename;
	char* str = "Dumping Ultralight%s%s Card Data...";	
	uint8_t *lockbytes_t = NULL;
	uint8_t lockbytes[2] = {0x00};
	uint8_t *lockbytes_t2 = NULL;
	uint8_t lockbytes2[2] = {0x00};
	bool bit[16]  = {0x00};
	bool bit2[16] = {0x00};
	uint8_t data[1024] = {0x00};
	bool hasPwd = false;
	int i = 0;
	int Pages = 16;
	bool tmplockbit = false;
	uint8_t dataLen=0;
	uint8_t cmdp =0;
	uint8_t key[16] = {0x00};
	uint8_t	*keyPtr = key;
	size_t fileNlen = 0;
	bool errors = false;
	bool swapEndian = false;

	while(param_getchar(Cmd, cmdp) != 0x00)
	{
		switch(param_getchar(Cmd, cmdp))
		{
		case 'h':
		case 'H':
			return usage_hf_mfu_dump();
		case 'k':
		case 'K':
			dataLen = param_gethex(Cmd, cmdp+1, data, 32);
			if (dataLen) {
				errors = true; 
			} else {
				memcpy(key, data, 16);
			}   
			cmdp += 2;
			hasPwd = true;
			break;
		case 'n':
		case 'N':
			fileNlen = param_getstr(Cmd, cmdp+1, filename);
			if (!fileNlen) errors = true; 
			if (fileNlen > FILE_PATH_SIZE-5) fileNlen = FILE_PATH_SIZE-5;
			cmdp += 2;
			break;
		case 's':
			swapEndian = true;
			cmdp++;
		default:
			PrintAndLog("Unknown parameter '%c'", param_getchar(Cmd, cmdp));
			errors = true;
			break;
		}
		if(errors) break;
	}

	//Validations
	if(errors) return usage_hf_mfu_dump();
	
	if (swapEndian)
		keyPtr = SwapEndian64(data, 16);

	TagTypeUL_t tagtype = GetHF14AMfU_Type();
	if (tagtype == UL_ERROR) return -1;
	
	if ( tagtype & UL ) {
		Pages = 16;
		PrintAndLog(str,"", (tagtype & MAGIC)?" (magic)":"" );
	}
	else if ( tagtype & UL_C ) {
		Pages = 44;
		PrintAndLog(str,"-C", (tagtype & MAGIC)?" (magic)":"" );
	}
	else if ( tagtype & UL_EV1_48 ) {
		Pages = 18; 
		PrintAndLog(str," EV1_48","");
	}
	else if ( tagtype & UL_EV1_128 ) {
		Pages = 32; 
		PrintAndLog(str," EV1_128","");
	} else {
		Pages = 16;
		PrintAndLog("Dumping unknown Ultralight, using default values.");
	}
	
	UsbCommand c = {CMD_MIFAREUC_READCARD, {0,Pages}};
	if ( hasPwd ) {
		c.arg[2] = 1;
		memcpy(c.d.asBytes, key, 16);
	}
	SendCommand(&c);
	UsbCommand resp;
	if (!WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
		PrintAndLog("Command execute time-out");
		return 1;
	}
	PrintAndLog	("%u,%u",resp.arg[0],resp.arg[1]);
	uint8_t isOK = resp.arg[0] & 0xff;
	if (isOK) {
		memcpy(data, resp.d.asBytes, resp.arg[1]);
	} else {
		PrintAndLog("Failed reading block: (%02x)", i);
		return 1;
	}

	// Load lock bytes.
	int j = 0;
	
	lockbytes_t = data + 8;
	lockbytes[0] = lockbytes_t[2];
	lockbytes[1] = lockbytes_t[3];
	for(j = 0; j < 16; j++){
		bit[j] = lockbytes[j/8] & ( 1 <<(7-j%8));
	}		
	
	// Load bottom lockbytes if available
	if ( Pages == 44 ) {
		lockbytes_t2 = data + (40*4);
		lockbytes2[0] = lockbytes_t2[2];
		lockbytes2[1] = lockbytes_t2[3];
		for (j = 0; j < 16; j++) {
			bit2[j] = lockbytes2[j/8] & ( 1 <<(7-j%8));
		}
	}

	// add keys
	if (hasPwd){
		memcpy(data + Pages*4, key, 16);
		Pages += 4;
	}
	for (i = 0; i < Pages; ++i) {
		if ( i < 3 ) {
			PrintAndLog("Block %02x:%s ", i,sprint_hex(data + i * 4, 4));
			continue;
		}
		switch(i){
			case 3: tmplockbit = bit[4]; break;
			case 4:	tmplockbit = bit[3]; break;
			case 5:	tmplockbit = bit[2]; break;
			case 6:	tmplockbit = bit[1]; break;
			case 7:	tmplockbit = bit[0]; break;
			case 8:	tmplockbit = bit[15]; break;
			case 9: tmplockbit = bit[14]; break;
			case 10: tmplockbit = bit[13]; break;
			case 11: tmplockbit = bit[12]; break;
			case 12: tmplockbit = bit[11]; break;
			case 13: tmplockbit = bit[10]; break;
			case 14: tmplockbit = bit[9]; break;
			case 15: tmplockbit = bit[8]; break;
			case 16:
			case 17:
			case 18:
			case 19: tmplockbit = bit2[6]; break;
			case 20:
			case 21:
			case 22:
			case 23: tmplockbit = bit2[5]; break; 
			case 24:
			case 25:
			case 26:
			case 27: tmplockbit = bit2[4]; break; 		    
			case 28:
			case 29:
			case 30:
			case 31: tmplockbit = bit2[2]; break;
			case 32:
			case 33:
			case 34:
			case 35: tmplockbit = bit2[1]; break; 
			case 36:
			case 37:
			case 38:
			case 39: tmplockbit = bit2[0]; break; 
			case 40: tmplockbit = bit2[12]; break;
			case 41: tmplockbit = bit2[11]; break;
			case 42: tmplockbit = bit2[10]; break; //auth0
			case 43: tmplockbit = bit2[9]; break;  //auth1
			default: break;
		}
		PrintAndLog("Block %02x:%s [%d]", i,sprint_hex(data + i * 4, 4),tmplockbit);
	}  
	
	// user supplied filename?
	if (fileNlen < 1) {
		// UID = data 0-1-2 4-5-6-7  (skips a beat)
		sprintf(fnameptr,"%02X%02X%02X%02X%02X%02X%02X.bin",
			data[0], data[1], data[2], data[4], data[5], data[6], data[7]);
	} else {
		sprintf(fnameptr + fileNlen," .bin");
	}

	if ((fout = fopen(filename,"wb")) == NULL) { 
		PrintAndLog("Could not create file name %s", filename);
		return 1;	
	}
	fwrite( data, 1, Pages*4, fout );
	fclose(fout);
	
	PrintAndLog("Dumped %d pages, wrote %d bytes to %s", Pages, Pages*4, filename);
	return 0;
}

// Needed to Authenticate to Ultralight C tags
void rol (uint8_t *data, const size_t len){
	uint8_t first = data[0];
	for (size_t i = 0; i < len-1; i++) {
		data[i] = data[i+1];
	}
	data[len-1] = first;
}

//-------------------------------------------------------------------------------
// Ultralight C Methods
//-------------------------------------------------------------------------------

//
// Ultralight C Authentication Demo {currently uses hard-coded key}
//
int CmdHF14AMfucAuth(const char *Cmd){

	uint8_t keyNo = 0;
	bool errors = false;

	char cmdp = param_getchar(Cmd, 0);

	//Change key to user defined one
	if (cmdp == 'k' || cmdp == 'K'){
		keyNo = param_get8(Cmd, 1);
		if(keyNo > 6) 
			errors = true;
	}

	if (cmdp == 'h' || cmdp == 'H')
		errors = true;
	
	if (errors) {
		PrintAndLog("Usage:  hf mfu cauth k <key number>");
		PrintAndLog("      0 (default): 3DES standard key");
		PrintAndLog("      1 : all 0x00 key");
		PrintAndLog("      2 : 0x00-0x0F key");
		PrintAndLog("      3 : nfc key");
		PrintAndLog("      4 : all 0x01 key");
		PrintAndLog("      5 : all 0xff key");
		PrintAndLog("      6 : 0x00-0xFF key");		
		PrintAndLog("\n      sample : hf mfu cauth k");
		PrintAndLog("               : hf mfu cauth k 3");
		return 0;
	} 

	uint8_t *key = default_3des_keys[keyNo];
	if (try3DesAuthentication(key)>0)
		PrintAndLog("Authentication successful. 3des key: %s",sprint_hex(key, 16));
	else
		PrintAndLog("Authentication failed");
			
	return 0;
}

int try3DesAuthentication( uint8_t *key){
	
	uint8_t blockNo = 0;
	uint32_t cuid = 0;

	des3_context ctx = { 0 };
	
	uint8_t random_a[8] = { 1,1,1,1,1,1,1,1 };
	uint8_t random_b[8] = { 0 };
	uint8_t enc_random_b[8] = { 0 };
	uint8_t rnd_ab[16] = { 0 };
	uint8_t iv[8] = { 0 };

	UsbCommand c = {CMD_MIFAREUC_AUTH1, {blockNo}};
	SendCommand(&c);
	UsbCommand resp;
	if ( !WaitForResponseTimeout(CMD_ACK, &resp, 1500) ) 	return -1;
	if ( !(resp.arg[0] & 0xff) ) return -2;
	
	cuid  = resp.arg[1];	
	memcpy(enc_random_b,resp.d.asBytes+1,8);

	des3_set2key_dec(&ctx, key);
	// context, mode, length, IV, input, output 
	des3_crypt_cbc( &ctx, DES_DECRYPT, sizeof(random_b), iv , enc_random_b , random_b);

	rol(random_b,8);
	memcpy(rnd_ab  ,random_a,8);
	memcpy(rnd_ab+8,random_b,8);
	
	//PrintAndLog("      RndA  :%s", sprint_hex(random_a, 8));
	//PrintAndLog("  enc(RndB) :%s", sprint_hex(enc_random_b, 8));
	//PrintAndLog("       RndB :%s", sprint_hex(random_b, 8));
	//PrintAndLog("        A+B :%s", sprint_hex(rnd_ab, 16));
	
	des3_set2key_enc(&ctx, key);
	// context, mode, length, IV, input, output 
	des3_crypt_cbc(&ctx, DES_ENCRYPT, sizeof(rnd_ab), enc_random_b, rnd_ab, rnd_ab);

	//Auth2
	c.cmd = CMD_MIFAREUC_AUTH2;
	c.arg[0] = cuid;
	memcpy(c.d.asBytes, rnd_ab, 16);
	SendCommand(&c);

	if ( !WaitForResponseTimeout(CMD_ACK, &resp, 1500)) return -1;				
	if ( !(resp.arg[0] & 0xff)) return -2;
	
	uint8_t enc_resp[8] = { 0 };
	uint8_t resp_random_a[8] = { 0 };
	memcpy(enc_resp, resp.d.asBytes+1, 8);

	des3_set2key_dec(&ctx, key);
	// context, mode, length, IV, input, output
	des3_crypt_cbc( &ctx, DES_DECRYPT, 8, enc_random_b, enc_resp, resp_random_a);

	//PrintAndLog("   enc(A+B) :%s", sprint_hex(rnd_ab, 16));
	//PrintAndLog(" enc(RndA') :%s", sprint_hex(enc_resp, 8));

	if ( !memcmp(resp_random_a, random_a, 8))
		return 1;	
	return 0;
}

/**
A test function to validate that the polarssl-function works the same 
was as the openssl-implementation. 
Commented out, since it requires openssl 

int CmdTestDES(const char * cmd)
{
	uint8_t key[16] = {0x00};	
	
	memcpy(key,key3_3des_data,16);  
	DES_cblock RndA, RndB;

	PrintAndLog("----------OpenSSL DES implementation----------");
	{
		uint8_t e_RndB[8] = {0x00};
		unsigned char RndARndB[16] = {0x00};

		DES_cblock iv = { 0 };
		DES_key_schedule ks1,ks2;
		DES_cblock key1,key2;

		memcpy(key,key3_3des_data,16);  
		memcpy(key1,key,8);
		memcpy(key2,key+8,8);


		DES_set_key((DES_cblock *)key1,&ks1);
		DES_set_key((DES_cblock *)key2,&ks2);

		DES_random_key(&RndA);
		PrintAndLog("     RndA:%s",sprint_hex(RndA, 8));
		PrintAndLog("     e_RndB:%s",sprint_hex(e_RndB, 8));
		//void DES_ede2_cbc_encrypt(const unsigned char *input,
		//    unsigned char *output, long length, DES_key_schedule *ks1,
		//    DES_key_schedule *ks2, DES_cblock *ivec, int enc);
		DES_ede2_cbc_encrypt(e_RndB,RndB,sizeof(e_RndB),&ks1,&ks2,&iv,0);

		PrintAndLog("     RndB:%s",sprint_hex(RndB, 8));
		rol(RndB,8);
		memcpy(RndARndB,RndA,8);
		memcpy(RndARndB+8,RndB,8);
		PrintAndLog("     RA+B:%s",sprint_hex(RndARndB, 16));
		DES_ede2_cbc_encrypt(RndARndB,RndARndB,sizeof(RndARndB),&ks1,&ks2,&e_RndB,1);
		PrintAndLog("enc(RA+B):%s",sprint_hex(RndARndB, 16));

	}
	PrintAndLog("----------PolarSSL implementation----------");
	{
		uint8_t random_a[8]     = { 0 };
		uint8_t enc_random_a[8] = { 0 };
		uint8_t random_b[8]     = { 0 };
		uint8_t enc_random_b[8] = { 0 };
		uint8_t random_a_and_b[16] = { 0 };
		des3_context ctx        = { 0 };

		memcpy(random_a, RndA,8);

		uint8_t output[8]       = { 0 };
		uint8_t iv[8]           = { 0 };

		PrintAndLog("     RndA  :%s",sprint_hex(random_a, 8));
		PrintAndLog("     e_RndB:%s",sprint_hex(enc_random_b, 8));

		des3_set2key_dec(&ctx, key);

		des3_crypt_cbc(&ctx      // des3_context *ctx
			, DES_DECRYPT        // int mode
			, sizeof(random_b)   // size_t length
			, iv                 // unsigned char iv[8]
			, enc_random_b       // const unsigned char *input
			, random_b           // unsigned char *output
			);

		PrintAndLog("     RndB:%s",sprint_hex(random_b, 8));

		rol(random_b,8);
		memcpy(random_a_and_b  ,random_a,8);
		memcpy(random_a_and_b+8,random_b,8);
		
		PrintAndLog("     RA+B:%s",sprint_hex(random_a_and_b, 16));

		des3_set2key_enc(&ctx, key);

		des3_crypt_cbc(&ctx          // des3_context *ctx
			, DES_ENCRYPT            // int mode
			, sizeof(random_a_and_b)   // size_t length
			, enc_random_b           // unsigned char iv[8]
			, random_a_and_b         // const unsigned char *input
			, random_a_and_b         // unsigned char *output
			);

		PrintAndLog("enc(RA+B):%s",sprint_hex(random_a_and_b, 16));
	}
	return 0;	
}
**/

//
// Ultralight C Read Single Block
//
int CmdHF14AMfUCRdBl(const char *Cmd)
{
	UsbCommand resp;
	bool hasPwd = FALSE;
	uint8_t blockNo = -1;
	uint8_t key[16];
	char cmdp = param_getchar(Cmd, 0);
	
	if (strlen(Cmd) < 1 || cmdp == 'h' || cmdp == 'H') {
		PrintAndLog("Usage:  hf mfu crdbl  <block number> <key>");
		PrintAndLog("");
		PrintAndLog("sample: hf mfu crdbl 0");
		PrintAndLog("        hf mfu crdbl 0 00112233445566778899AABBCCDDEEFF");
		return 0;
	}       
		
	blockNo = param_get8(Cmd, 0);
	if (blockNo < 0) {
		PrintAndLog("Wrong block number");
		return 1;
	}
	
	if (blockNo > MAX_ULC_BLOCKS ){
		PrintAndLog("Error: Maximum number of blocks is 47 for Ultralight-C");
		return 1;
	} 
	
	// key
	if ( strlen(Cmd) > 3){
		if (param_gethex(Cmd, 1, key, 32)) {
			PrintAndLog("Key must include %d HEX symbols", 32);
			return 1;
		} else {
			hasPwd = TRUE;
		}	
	}	

	//Read Block
	UsbCommand c = {CMD_MIFAREU_READBL, {blockNo}};
	if ( hasPwd ) {
		c.arg[1] = 1;
		memcpy(c.d.asBytes,key,16);
	}
	SendCommand(&c);

	if (WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
		uint8_t isOK = resp.arg[0] & 0xff;
		if (isOK) {
			uint8_t *data = resp.d.asBytes;
			PrintAndLog("Block: %0d (0x%02X) [ %s]", (int)blockNo, blockNo, sprint_hex(data, 4));
		}
		else {
			PrintAndLog("Failed reading block: (%02x)", isOK);
		}
	} else {
		PrintAndLog("Command execute time-out");
	}
	return 0;
}

//
//  Mifare Ultralight C Write Single Block
//
int CmdHF14AMfUCWrBl(const char *Cmd){
	
	uint8_t blockNo = -1;
	bool chinese_card = FALSE;
	uint8_t bldata[16] = {0x00};
	UsbCommand resp;

	char cmdp = param_getchar(Cmd, 0);
	
	if (strlen(Cmd) < 3 || cmdp == 'h' || cmdp == 'H') {	
		PrintAndLog("Usage:  hf mfu cwrbl <block number> <block data (8 hex symbols)> [w]");
		PrintAndLog("       [block number]");
		PrintAndLog("       [block data] - (8 hex symbols)");
		PrintAndLog("       [w] - Chinese magic ultralight tag");
		PrintAndLog("");
		PrintAndLog("        sample: hf mfu cwrbl 0 01020304");
		PrintAndLog("");
		return 0;
	}
	
	blockNo = param_get8(Cmd, 0);
	if (blockNo > MAX_ULC_BLOCKS ){
		PrintAndLog("Error: Maximum number of blocks is 47 for Ultralight-C Cards!");
		return 1;
	}
	
	if (param_gethex(Cmd, 1, bldata, 8)) {
		PrintAndLog("Block data must include 8 HEX symbols");
		return 1;
	}
	
	if (strchr(Cmd,'w') != 0  || strchr(Cmd,'W') != 0 ) {
		chinese_card = TRUE; 
	}
	
	if ( blockNo <= 3 ) {
		if (!chinese_card){
			 PrintAndLog("Access Denied");  
			return 1;
		} else {
			PrintAndLog("--Special block no: 0x%02x", blockNo);
			PrintAndLog("--Data: %s", sprint_hex(bldata, 4));
			UsbCommand d = {CMD_MIFAREU_WRITEBL, {blockNo}};
			memcpy(d.d.asBytes,bldata, 4);
			SendCommand(&d);
			if (WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
				uint8_t isOK  = resp.arg[0] & 0xff;
				PrintAndLog("isOk:%02x", isOK);
			} else {
				PrintAndLog("Command execute timeout");
				return 1;
			}  
		}	
	} else {
			PrintAndLog("--Block no : 0x%02x", blockNo);
			PrintAndLog("--Data: %s", sprint_hex(bldata, 4));        	
			UsbCommand e = {CMD_MIFAREU_WRITEBL, {blockNo}};
			memcpy(e.d.asBytes,bldata, 4);
			SendCommand(&e);
			if (WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
				uint8_t isOK  = resp.arg[0] & 0xff;
				PrintAndLog("isOk : %02x", isOK);
			} else {
				PrintAndLog("Command execute timeout");
				return 1;
			}
	}
	return 0;
}

// 
// Mifare Ultralight C - Set password
//
int CmdHF14AMfucSetPwd(const char *Cmd){

	uint8_t pwd[16] = {0x00};	
	
	char cmdp = param_getchar(Cmd, 0);
	
	if (strlen(Cmd) == 0  || cmdp == 'h' || cmdp == 'H') {	
		PrintAndLog("Usage:  hf mfu setpwd <password (32 hex symbols)>");
		PrintAndLog("       [password] - (32 hex symbols)");
		PrintAndLog("");
		PrintAndLog("sample: hf mfu setpwd 000102030405060708090a0b0c0d0e0f");
		PrintAndLog("");
		return 0;
	}
	
	if (param_gethex(Cmd, 0, pwd, 32)) {
		PrintAndLog("Password must include 32 HEX symbols");
		return 1;
	}
	
	UsbCommand c = {CMD_MIFAREUC_SETPWD};	
	memcpy( c.d.asBytes, pwd, 16);
	SendCommand(&c);

	UsbCommand resp;
	
	if (WaitForResponseTimeout(CMD_ACK,&resp,1500) ) {
		if ( (resp.arg[0] & 0xff) == 1)
			PrintAndLog("Ultralight-C new password: %s", sprint_hex(pwd,16));
		else{
			PrintAndLog("Failed writing at block %d", resp.arg[1] & 0xff);
			return 1;
		}
	}
	else {
		PrintAndLog("command execution time out");
		return 1;
	}
	
	return 0;
}

//
// Magic UL / UL-C tags  - Set UID
//
int CmdHF14AMfucSetUid(const char *Cmd){

	UsbCommand c;
	UsbCommand resp;
	uint8_t uid[7] = {0x00};
	char cmdp = param_getchar(Cmd, 0);
	
	if (strlen(Cmd) == 0  || cmdp == 'h' || cmdp == 'H') {	
		PrintAndLog("Usage:  hf mfu setuid <uid (14 hex symbols)>");
		PrintAndLog("       [uid] - (14 hex symbols)");
		PrintAndLog("\nThis only works for Magic Ultralight tags.");
		PrintAndLog("");
		PrintAndLog("sample: hf mfu setuid 11223344556677");
		PrintAndLog("");
		return 0;
	}
	
	if (param_gethex(Cmd, 0, uid, 14)) {
		PrintAndLog("UID must include 14 HEX symbols");
		return 1;
	}

	// read block2. 
	c.cmd = CMD_MIFAREU_READBL;
	c.arg[0] = 2;
	SendCommand(&c);
	if (!WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
		PrintAndLog("Command execute timeout");
		return 2;
	}
	
	// save old block2.
	uint8_t oldblock2[4] = {0x00};
	memcpy(resp.d.asBytes, oldblock2, 4);
	
	// block 0.
	c.cmd = CMD_MIFAREU_WRITEBL;
	c.arg[0] = 0;
	c.d.asBytes[0] = uid[0];
	c.d.asBytes[1] = uid[1];
	c.d.asBytes[2] = uid[2];
	c.d.asBytes[3] =  0x88 ^ uid[0] ^ uid[1] ^ uid[2];
	SendCommand(&c);
	if (!WaitForResponseTimeout(CMD_ACK,&resp,1500)) {
		PrintAndLog("Command execute timeout");
		return 3;
	}
	
	// block 1.
	c.arg[0] = 1;
	c.d.asBytes[0] = uid[3];
	c.d.asBytes[1] = uid[4];
	c.d.asBytes[2] = uid[5];
	c.d.asBytes[3] = uid[6];
	SendCommand(&c);
	if (!WaitForResponseTimeout(CMD_ACK,&resp,1500) ) {
		PrintAndLog("Command execute timeout");
		return 4;
	}

	// block 2.
	c.arg[0] = 2;
	c.d.asBytes[0] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
	c.d.asBytes[1] = oldblock2[1];
	c.d.asBytes[2] = oldblock2[2];
	c.d.asBytes[3] = oldblock2[3];
	SendCommand(&c);
	if (!WaitForResponseTimeout(CMD_ACK,&resp,1500) ) {
		PrintAndLog("Command execute timeout");
		return 5;
	}
	
	return 0;
}

int CmdHF14AMfuGenDiverseKeys(const char *Cmd){
	
	uint8_t iv[8] = { 0x00 };
	uint8_t block = 0x07;
	
	// UL-EV1
	//04 57 b6 e2 05 3f 80 UID
	//4a f8 4b 19   PWD
	uint8_t uid[] = { 0xF4,0xEA, 0x54, 0x8E };
	uint8_t mifarekeyA[] = { 0xA0,0xA1,0xA2,0xA3,0xA4,0xA5 };
	uint8_t mifarekeyB[] = { 0xB0,0xB1,0xB2,0xB3,0xB4,0xB5 };
	uint8_t dkeyA[8] = { 0x00 };
	uint8_t dkeyB[8] = { 0x00 };
	
	uint8_t masterkey[] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
	
	uint8_t mix[8] = { 0x00 };
	uint8_t divkey[8] = { 0x00 };
	
	memcpy(mix, mifarekeyA, 4);
	
	mix[4] = mifarekeyA[4] ^ uid[0];
	mix[5] = mifarekeyA[5] ^ uid[1];
	mix[6] = block ^ uid[2];
	mix[7] = uid[3];
	
	des3_context ctx = { 0x00 };
	des3_set2key_enc(&ctx, masterkey);

	des3_crypt_cbc(&ctx  // des3_context
		, DES_ENCRYPT    // int mode
		, sizeof(mix)    // length
		, iv             // iv[8]
		, mix            // input
		, divkey         // output
		);

	PrintAndLog("3DES version");
	PrintAndLog("Masterkey    :\t %s", sprint_hex(masterkey,sizeof(masterkey)));
	PrintAndLog("UID          :\t %s", sprint_hex(uid, sizeof(uid)));
	PrintAndLog("Sector       :\t %0d", block);
	PrintAndLog("Mifare key   :\t %s", sprint_hex(mifarekeyA, sizeof(mifarekeyA)));
	PrintAndLog("Message      :\t %s", sprint_hex(mix, sizeof(mix)));
	PrintAndLog("Diversified key: %s", sprint_hex(divkey+1, 6));
		
	PrintAndLog("\n DES version");
	
	for (int i=0; i < sizeof(mifarekeyA); ++i){
		dkeyA[i] = (mifarekeyA[i] << 1) & 0xff;
		dkeyA[6] |=  ((mifarekeyA[i] >> 7) & 1) << (i+1);
	}
	
	for (int i=0; i < sizeof(mifarekeyB); ++i){
		dkeyB[1] |=  ((mifarekeyB[i] >> 7) & 1) << (i+1);
		dkeyB[2+i] = (mifarekeyB[i] << 1) & 0xff;
	}
	
	uint8_t zeros[8] = {0x00};
	uint8_t newpwd[8] = {0x00};
	uint8_t dmkey[24] = {0x00};
	memcpy(dmkey, dkeyA, 8);
	memcpy(dmkey+8, dkeyB, 8);
	memcpy(dmkey+16, dkeyA, 8);
	memset(iv, 0x00, 8);
	
	des3_set3key_enc(&ctx, dmkey);

	des3_crypt_cbc(&ctx  // des3_context
		, DES_ENCRYPT    // int mode
		, sizeof(newpwd) // length
		, iv             // iv[8]
		, zeros         // input
		, newpwd         // output
		);
	
	PrintAndLog("Mifare dkeyA :\t %s", sprint_hex(dkeyA, sizeof(dkeyA)));
	PrintAndLog("Mifare dkeyB :\t %s", sprint_hex(dkeyB, sizeof(dkeyB)));
	PrintAndLog("Mifare ABA   :\t %s", sprint_hex(dmkey, sizeof(dmkey)));
	PrintAndLog("Mifare Pwd   :\t %s", sprint_hex(newpwd, sizeof(newpwd)));
	
	return 0;
}

// static uint8_t * diversify_key(uint8_t * key){
	
 // for(int i=0; i<16; i++){
   // if(i<=6) key[i]^=cuid[i];
   // if(i>6) key[i]^=cuid[i%7];
 // }
 // return key;
// }

// static void GenerateUIDe( uint8_t *uid, uint8_t len){
	// for (int i=0; i<len; ++i){
			
	// }
	// return;
// }

static command_t CommandTable[] =
{
	{"help",	CmdHelp,			1, "This help"},
	{"dbg",		CmdHF14AMfDbg,		0, "Set default debug mode"},
	{"info",	CmdHF14AMfUInfo,	0, "Tag information"},
	{"dump",	CmdHF14AMfUDump,	0, "Dump Ultralight / Ultralight-C tag to binary file"},
	{"rdbl",	CmdHF14AMfURdBl,	0, "Read block  - Ultralight"},
	{"wrbl",	CmdHF14AMfUWrBl,	0, "Write block - Ultralight"},    
	{"crdbl",	CmdHF14AMfUCRdBl,	0, "Read block        - Ultralight C"},
	{"cwrbl",	CmdHF14AMfUCWrBl,	0, "Write block       - Ultralight C"},
	{"cauth",	CmdHF14AMfucAuth,	0, "Authentication    - Ultralight C"},
	{"setpwd",	CmdHF14AMfucSetPwd, 1, "Set 3des password - Ultralight-C"},
	{"setuid",	CmdHF14AMfucSetUid, 1, "Set UID - MAGIC tags only"},
	{"gen",		CmdHF14AMfuGenDiverseKeys , 1, "Generate 3des mifare diversified keys"},
	{NULL, NULL, 0, NULL}
};

int CmdHFMFUltra(const char *Cmd){
	WaitForResponseTimeout(CMD_ACK,NULL,100);
	CmdsParse(CommandTable, Cmd);
	return 0;
}

int CmdHelp(const char *Cmd){
	CmdsHelp(CommandTable);
	return 0;
}
