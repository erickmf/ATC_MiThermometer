/*
 * mi_beacon.c
 *
 *  Created on: 16.02.2021
 *      Author: pvvx
 */
#include <stdint.h>
#include "tl_common.h"
#if USE_MIHOME_BEACON
//#include "drivers.h"
#include "app_config.h"
#include "ble.h"
#include "cmd_parser.h"
#include "battery.h"
#include "app.h"
#include "flash_eep.h"
#if	USE_TRIGGER_OUT
#include "trigger.h"
#endif
#if USE_FLASH_MEMO
#include "logger.h"
#endif
#include "ccm.h"

/* Encrypted mi beacon structs */
typedef union _adv_mi_fctrl_t { // Frame Control
	struct __attribute__((packed)) {
		uint16_t Factory: 		1; // reserved text
		uint16_t Connected: 	1; // reserved text
		uint16_t Central: 		1; // Keep
		uint16_t isEncrypted: 	1; // 0: The package is not encrypted; 1: The package is encrypted
		uint16_t MACInclude: 	1; // 0: Does not include the MAC address; 1: includes a fixed MAC address (the MAC address is included for iOS to recognize this device and connect)
		uint16_t CapabilityInclude: 	1; // 0: does not include Capability; 1: includes Capability. Before the device is bound, this bit is forced to 1
		uint16_t ObjectInclude:	1; // 0: does not contain Object; 1: contains Object
		uint16_t Mesh: 			1; // 0: does not include Mesh; 1: includes Mesh. For standard BLE access products and high security level access, this item is mandatory to 0. This item is mandatory for Mesh access to 1.
		uint16_t registered:	1; // 0: The device is not bound; 1: The device is registered and bound.
		uint16_t solicited:		1; // 0: No operation; 1: Request APP to register and bind. It is only valid when the user confirms the pairing by selecting the device on the developer platform, otherwise set to 0. The original name of this item was bindingCfm, and it was renamed to solicited "actively request, solicit" APP for registration and binding
		uint16_t AuthMode:		2; // 0: old version certification; 1: safety certification; 2: standard certification; 3: reserved
		uint16_t version:		4; // Version number (currently v5)
	} bit;
	uint16_t	word;	// = 0x3050 Frame ctrl
} adv_mi_fctrl_t; // Frame Control

typedef struct __attribute__((packed)) _adv_mi_enc_t {
	uint8_t		size;	// = 27?
	uint8_t		uid;	// = 0x16, 16-bit UUID https://www.bluetooth.com/specifications/assigned-numbers/generic-access-profile/
	uint16_t	UUID;	// = 0xFE95, 16-bit UUID for Members 0xFE95 Xiaomi Inc.
	adv_mi_fctrl_t fctrl;	// Frame Control
	uint16_t    dev_id;		// Device type (enum: XIAOMI_DEV_ID)
	uint8_t		counter;	// 0..0xff Measurement count, Serial number, used for de-duplication, different event or attribute reporting requires different Frame Counter
	uint8_t		MAC[6];		// [0] - lo, .. [6] - hi digits
	union {
		struct {
			uint16_t     data_id; 	// = 0x1004, 0x1006, 0x100a (XIAOMI_DATA_ID)
			uint8_t 	 data_len;
		};
		uint8_t 	 capability;
	};
} adv_mi_enc_t, * padv_mi_enc_t;

typedef struct __attribute__((packed)) _beacon_nonce_t{
    uint8_t  mac[6];
	uint16_t pid;
	union {
		struct {
			uint8_t  cnt;
			uint8_t  ext_cnt[3];
		};
		uint32_t cnt32;
    };
} beacon_nonce_t, * pbeacon_nonce_t;

/* Encrypted custom beacon structs */
typedef struct __attribute__((packed)) _adv_cust_head_t {
	uint8_t		size;		//@0 = 11
	uint8_t		uid;		//@1 = 0x16, 16-bit UUID https://www.bluetooth.com/specifications/assigned-numbers/generic-access-profile/
	uint16_t	UUID;		//@2..3 = GATT Service 0x181A Environmental Sensing (little-endian) (or 0x181C 'User Data'?)
	uint8_t		counter;	//@4 0..0xff Measurement count, Serial number, used for de-duplication, different event or attribute reporting requires different Frame Counter
} adv_cust_head_t, * padv_cust_head_t;

typedef struct __attribute__((packed)) _adv_cust_data_t {
	int16_t		temp;		//@0
	uint16_t	humi;		//@2
	uint8_t		bat;		//@4
	uint8_t		trg;		//@5
} adv_cust_data_t, * padv_cust_data_t;

typedef struct __attribute__((packed)) _adv_cust_enc_t {
	adv_cust_head_t head;
	adv_cust_data_t data;   //@5
	uint8_t		mic[4];		//@8..11
} adv_cust_enc_t, * padv_cust_enc_t;

/* Encrypted atc beacon structs
 * https://github.com/pvvx/ATC_MiThermometer/issues/94#issuecomment-842846036 */
typedef struct __attribute__((packed)) _adv_atc_data_t {
	uint8_t		temp;		//@0
	uint8_t		humi;		//@1
	uint8_t		bat;		//@2
} adv_atc_data_t, * padv_atc_data_t;

typedef struct __attribute__((packed)) _adv_atc_enc_t {
	adv_cust_head_t head;
	adv_atc_data_t data;   //@5
	uint8_t		mic[4];		//@8..11
} adv_atc_enc_t, * padv_atc_enc_t;

/* Encrypted atc/custom nonce */
typedef struct __attribute__((packed)) _enc_beacon_nonce_t{
    uint8_t  MAC[6];
    adv_cust_head_t head;
} enc_beacon_nonce_t;


//// Init data
RAM uint8_t bindkey[16];
RAM beacon_nonce_t beacon_nonce;
//// Counters
RAM uint32_t adv_mi_cnt = 0xffffffff; // counter of measurement numbers from sensors
//// Buffers
RAM uint8_t adv_crypt_buf[ADV_BUFFER_SIZE];
/// Vars
typedef struct _mi_beacon_data_t { // out data
	int16_t temp;	// x0.1 C
	uint16_t humi;	// x0.1 %
	uint8_t batt;	// 0..100 %
} mi_beacon_data_t;
RAM mi_beacon_data_t mi_beacon_data;

typedef struct _summ_data_t { // calk summ data
	uint32_t	batt; // mv
	int32_t		temp; // x 0.01 C
	uint32_t	humi; // x 0.01 %
	uint32_t 	count;
} mib_summ_data_t;
RAM mib_summ_data_t mib_summ_data;

/* Initializing data for mi beacon */
void mi_beacon_init(void) {
	uint8_t *p_key = find_mi_keys(MI_KEYTBIND_ID, 1);
	if(p_key) {
		memcpy(&bindkey, p_key + 12, sizeof(bindkey)); 
		p_key = find_mi_keys(MI_KEYSEQNUM_ID, 1);
		if(p_key)
			memcpy(&beacon_nonce.cnt32, p_key, 4); // BLE_GAP_AD_TYPE_FLAGS
	} else {
		if(flash_read_cfg(&bindkey, EEP_ID_KEY, sizeof(bindkey)) != sizeof(bindkey)) {
			generateRandomNum(sizeof(bindkey), (unsigned char *)&bindkey);
			flash_write_cfg(&bindkey, EEP_ID_KEY, sizeof(bindkey));
		}
	}
	memcpy(beacon_nonce.mac, mac_public, 6);
	beacon_nonce.pid = DEVICE_TYPE;
}

/* Averaging measurements */
_attribute_ram_code_
void mi_beacon_summ(void) {
	mib_summ_data.temp += measured_data.temp;
	mib_summ_data.humi += measured_data.humi;
	mib_summ_data.batt += measured_data.battery_mv;
	mib_summ_data.count++;
}

/* Create encrypted custom beacon packet
 * https://github.com/pvvx/ATC_MiThermometer/issues/94#issuecomment-842846036 */
__attribute__((optimize("-Os")))
void atc_encrypt_beacon(uint32_t cnt) {
	if(adv_mi_cnt != cnt) { // measurement counter update?
		adv_mi_cnt = cnt; // new counter
		padv_atc_enc_t p = (padv_atc_enc_t)&adv_crypt_buf;
		enc_beacon_nonce_t cbn;
		adv_atc_data_t data;
		uint8_t aad = 0x11;
		p->head.size = sizeof(adv_atc_enc_t) - 1;
		p->head.uid = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
		p->head.UUID = 0x181A; // GATT Service 0x181A Environmental Sensing (little-endian) (or 0x181C 'User Data'?)
		p->head.counter = (uint8_t)cnt;
		data.temp = measured_data.temp / 50 + 4000 / 50;
		data.humi = measured_data.humi / 50;
		data.bat = battery_level | ((trg.flg.trigger_on)? 0x80 : 0);

		memcpy(&cbn.MAC, mac_public, sizeof(cbn.MAC));
		memcpy(&cbn.head, p, sizeof(cbn.head));
		aes_ccm_encrypt_and_tag((const unsigned char *)&bindkey,
						   (uint8_t*)&cbn, sizeof(cbn),
						   &aad, sizeof(aad),
						   (uint8_t *)&data, sizeof(data),
						   (uint8_t *)&p->data,
						   p->mic, 4);
	}
	memcpy(&adv_buf.data, &adv_crypt_buf, sizeof(adv_atc_enc_t));
}

__attribute__((optimize("-Os")))
void pvvx_encrypt_beacon(uint32_t cnt) {
	if(adv_mi_cnt != cnt) { // measurement counter update?
		adv_mi_cnt = cnt; // new counter
		padv_cust_enc_t p = (padv_cust_enc_t)&adv_crypt_buf;
		enc_beacon_nonce_t cbn;
		adv_cust_data_t data;
		uint8_t aad = 0x11;
		p->head.size = sizeof(adv_cust_enc_t) - 1;
		p->head.uid = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
		p->head.UUID = 0x181A; // GATT Service 0x181A Environmental Sensing (little-endian) (or 0x181C 'User Data'?)
		p->head.counter = (uint8_t)cnt;
		data.temp = measured_data.temp;
		data.humi = measured_data.humi;
		data.bat = battery_level;
		data.trg = trg.flg_byte;
		memcpy(&cbn.MAC, mac_public, sizeof(cbn.MAC));
		memcpy(&cbn.head, p, sizeof(cbn.head));
		aes_ccm_encrypt_and_tag((const unsigned char *)&bindkey,
						   (uint8_t*)&cbn, sizeof(cbn),
						   &aad, sizeof(aad),
						   (uint8_t *)&data, sizeof(data),
						   (uint8_t *)&p->data,
						   p->mic, 4);
	}
	memcpy(&adv_buf.data, &adv_crypt_buf, sizeof(adv_cust_enc_t));
}

/* Create encrypted mi beacon packet */
__attribute__((optimize("-Os")))
void mi_encrypt_beacon(uint32_t cnt) {
	if(adv_mi_cnt != cnt) { // measurement counter update?
		adv_mi_cnt = cnt; // new counter
		beacon_nonce.cnt32 = cnt;
		if((cnt & 3) == 0) { // Data are averaged over a period of 16 measurements (cnt*4)
			mi_beacon_data.temp = ((int16_t)(mib_summ_data.temp/(int32_t)mib_summ_data.count))/10;
			mi_beacon_data.humi = ((uint16_t)(mib_summ_data.humi/mib_summ_data.count))/10;
			mi_beacon_data.batt = get_battery_level((uint16_t)(mib_summ_data.batt/mib_summ_data.count));
			memset(&mib_summ_data, 0, sizeof(mib_summ_data));
		}
		padv_mi_enc_t p = (padv_mi_enc_t)&adv_crypt_buf;
		p->uid = GAP_ADTYPE_SERVICE_DATA_UUID_16BIT; // 16-bit UUID
		p->UUID = 0xFE95; // 16-bit UUID for Members 0xFE95 Xiaomi Inc.
		p->dev_id = beacon_nonce.pid;
		p->counter = cnt;
		memcpy(p->MAC, mac_public, 6);
		uint8_t *mic = (uint8_t *)p;
		mic += sizeof(adv_mi_enc_t);
		switch(cnt & 3) {
			case 0:
				p->data_id = XIAOMI_DATA_ID_Temperature; // XIAOMI_DATA_ID
				p->data_len = 2;
				*mic++ = mi_beacon_data.temp;	// Temperature, Range: -400..+1000 (x0.1 C)
				*mic++ = mi_beacon_data.temp >> 8;
				break;
			case 1:
#if 0
				p->fctrl.word = 0;
				p->fctrl.bit.MACInclude = 1;
				p->fctrl.bit.CapabilityInclude = 1;
				p->fctrl.bit.registered = 1;
				p->fctrl.bit.AuthMode = 2;
				p->fctrl.bit.version = 5; // XIAOMI_DEV_VERSION
#else
				p->fctrl.word = 0x5830; // 0x5830
#endif
				p->capability = 0x08; // capability
				p->size = sizeof(adv_mi_enc_t) - 2 - 1;
				return;
			case 2:
				p->data_id = XIAOMI_DATA_ID_Humidity; // byte XIAOMI_DATA_ID
				p->data_len = 2;
				*mic++ = mi_beacon_data.humi; // Humidity percentage, Range: 0..1000 (x0.1 %)
				*mic++ = mi_beacon_data.humi >> 8;
				break;
			case 3:
				p->data_id = XIAOMI_DATA_ID_Power; // XIAOMI_DATA_ID
				p->data_len = 1;
				*mic++ = mi_beacon_data.batt; // Battery percentage, Range: 0..100 %
				break;
		}
#if 0
		p->fctrl.word = 0;
		p->fctrl.bit.isEncrypted = 1;
		p->fctrl.bit.MACInclude = 1;
		p->fctrl.bit.ObjectInclude = 1;
		p->fctrl.bit.registered = 1;
		p->fctrl.bit.AuthMode = 2;
		p->fctrl.bit.version = 5; // XIAOMI_DEV_VERSION
#else
		p->fctrl.word = 0x5858; // 0x5858
#endif
		p->size = p->data_len + sizeof(adv_mi_enc_t) + 3 + 4 - 1; // + counter bit8..31 + mic 32 bits
		*mic++ = beacon_nonce.ext_cnt[0];
		*mic++ = beacon_nonce.ext_cnt[1];
		*mic++ = beacon_nonce.ext_cnt[2];
	    uint8_t aad = 0x11;
#if 0
	    ccm_auth_crypt(0, (const unsigned char *)&bindkey,
				   (uint8_t*)&beacon_nonce, sizeof(beacon_nonce),
				   &aad, sizeof(aad),
				   (uint8_t *)&p->data_id, p->data_len + 3,
				   (uint8_t *)&p->data_id,
				   mic, 4);
#else
		aes_ccm_encrypt_and_tag((const unsigned char *)&bindkey,
							   (uint8_t*)&beacon_nonce, sizeof(beacon_nonce),
							   &aad, sizeof(aad),
							   (uint8_t *)&p->data_id, p->data_len + 3,
							   (uint8_t *)&p->data_id,
							   mic, 4);
#endif
	}
	memcpy(&adv_buf.data, &adv_crypt_buf, min(sizeof(adv_crypt_buf), ADV_BUFFER_SIZE));
}

#endif // USE_MIHOME_BEACON
