/*                    The Quest Operating System
 *  Copyright (C) 2005-2012  Richard West, Boston University
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* USB Mass Storage Class driver */
#include "drivers/usb/usb.h"
#include "util/printf.h"
#include "sched/vcpu.h"
#include "sched/sched.h"
#include "drivers/usb/ehci.h"
#include "kernel.h"


#define USB_MASS_STORAGE_CLASS 0x8
#define UMSC_PROTOCOL 0x50

//#define DEBUG_UMSC

#ifdef DEBUG_UMSC
#define DLOG(fmt,...) DLOG_PREFIX("umsc",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

#define UMSC_CBW_SIGNATURE 0x43425355 /* "USBC" (little-endian) */
struct umsc_cbw {
  uint32 dCBWSignature;
  uint32 dCBWTag;
  uint32 dCBWDataTransferLength;
  union {
    uint8 raw;
    struct {
      uint8 _reserved:7;
      uint8 direction:1;
    };
  } bmCBWFlags;
  uint8 bCBWLUN:4;
  uint8 _reserved1:4;
  uint8 bCBWCBLength:5;
  uint8 _reserved2:3;
  uint8 CBWCB[16];
} PACKED;
typedef struct umsc_cbw UMSC_CBW;

#define UMSC_CSW_SIGNATURE 0x53425355 /* "USBS" (little-endian) */
struct umsc_csw {
  uint32 dCSWSignature;
  uint32 dCSWTag;
  uint32 dCSWDataResidue;
  uint8  bCSWStatus;
} PACKED;
typedef struct umsc_csw UMSC_CSW;

static uint testepout, testepin;
static USB_DEVICE_INFO* testinfo;


typedef struct {
  USB_DEVICE_INFO *devinfo;
  uint ep_out, ep_in, maxpkt, last_lba, sector_size;
} umsc_device_t;

#define UMSC_MAX_DEVICES 16
static umsc_device_t umsc_devs[UMSC_MAX_DEVICES];
static uint num_umsc_devs=0;

sint
umsc_bulk_scsi (USB_DEVICE_INFO* info, uint ep_out, uint ep_in,
                uint8 cmd[16], uint dir, uint8* data,
                uint data_len, uint maxpkt)
{
  UMSC_CBW cbw;
  UMSC_CSW csw;
  sint status;
  int act_len;


  DLOG ("cmd: %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X %.02X",
        cmd[0], cmd[1], cmd[2], cmd[3],
        cmd[4], cmd[5], cmd[6], cmd[7],
        cmd[8], cmd[9], cmd[10], cmd[11],
        cmd[12], cmd[13], cmd[14], cmd[15]);

  memset (&cbw, 0, sizeof (cbw));
  memset (&csw, 0, sizeof (csw));

  cbw.dCBWSignature = UMSC_CBW_SIGNATURE;
  cbw.dCBWDataTransferLength = data_len;
  cbw.bmCBWFlags.direction = dir;
  cbw.bCBWCBLength = 16;            /* cmd length */
  memcpy (cbw.CBWCB, cmd, 16);
  status = usb_bulk_msg(info, usb_sndbulkpipe(info, ep_out), &cbw, 0x1F,
                        &act_len, USB_DEFAULT_CONTROL_MSG_TIMEOUT);


  DLOG("%d:status=%d, data_len=%d, act_len=%d", __LINE__, status, 0x1F, act_len);

  
  if(status < 0) return status;
  

  if (data_len > 0) {
    if (dir) {
      status = usb_bulk_msg(info, usb_rcvbulkpipe(info, ep_in), data, data_len, &act_len,
                                 USB_DEFAULT_BULK_MSG_TIMEOUT);
    }
    else {
      status = usb_bulk_msg(info, usb_sndbulkpipe(info, ep_out), data, data_len, &act_len,
                                 USB_DEFAULT_BULK_MSG_TIMEOUT);
    }

    DLOG("%d:status=%d, data_len=%d, act_len=%d", __LINE__, status, data_len, act_len);

    if (status < 0) return status;

    DLOG ("data=%.02X %.02X %.02X %.02X", data[0], data[1], data[2], data[3]);

  }
  status = usb_bulk_msg(info, usb_rcvbulkpipe(info, ep_in), &csw, 0x0d, &act_len,
                        USB_DEFAULT_CONTROL_MSG_TIMEOUT);


  DLOG("%d:status=%d, data_len=%d, act_len=%d", __LINE__, status, 0x0d, act_len);

  if (status < 0) return status;

  DLOG ("csw sig=%p tag=%p res=%d status=%d",
        csw.dCSWSignature, csw.dCSWTag, csw.dCSWDataResidue, csw.bCSWStatus);

  return status;
}

sint
umsc_read_sector (uint dev_index, uint32 lba, uint8 *sector, uint len)
{
  umsc_device_t *umsc;
  
  if (dev_index >= num_umsc_devs) return 0;
  if (len < umsc_devs[dev_index].sector_size) return 0;
  
  uint8 cmd[16] = { [0] = 0x28,
                    [2] = (lba >> 0x18) & 0xFF,
                    [3] = (lba >> 0x10) & 0xFF,
                    [4] = (lba >> 0x08) & 0xFF,
                    [5] = (lba >> 0x00) & 0xFF,
                    [8] = 1 };
  if (dev_index >= num_umsc_devs) return 0;
  umsc = &umsc_devs[dev_index];
  if (len < umsc->sector_size) return 0;
  DLOG("In %s", __FUNCTION__);
  if (umsc_bulk_scsi (umsc->devinfo,
                      umsc->ep_out, umsc->ep_in, cmd, 1, sector,
                      umsc->sector_size, umsc->maxpkt) < 0)
    return 0;
  return umsc->sector_size;
}

int
umsc_read_sectors (uint dev_index, uint32 lba, uint8 * buf, uint16 snum)
{
  umsc_device_t * umsc;
  uint8 * index = buf;
  uint16 res = 0, sectors = 0;
  uint32 total_sec = 0;
  int rds = 0;
  uint8 cmd[16] = { [0] = 0x28,
                    [2] = (lba >> 0x18) & 0xFF,
                    [3] = (lba >> 0x10) & 0xFF,
                    [4] = (lba >> 0x08) & 0xFF,
                    [5] = (lba >> 0x00) & 0xFF };

  if (dev_index >= num_umsc_devs) return 0;
  umsc = &umsc_devs[dev_index];

  rds = snum / 0xFFFF;
  res = snum % 0xFFFF;

  do {
    sectors = (rds) ? 0xFFFF : res;
    cmd[7] = (sectors >> 0x08) & 0xFF;  /* MSB of length */
    cmd[8] = (sectors >> 0x00) & 0xFF;  /* LSB of length */
    if (umsc_bulk_scsi (umsc->devinfo,
                        umsc->ep_out, umsc->ep_in, cmd, 1,
                        index + umsc->sector_size * total_sec,
                        umsc->sector_size * sectors, umsc->maxpkt) < 0)
      return total_sec;
    total_sec += sectors;
  } while ((--rds) >= 0);

  return total_sec;
}

extern void
umsc_tmr_test (void)
{
  #if 1
  uint8 conf[16];
  USB_DEVICE_INFO* info = testinfo;
  uint  ep_out = testepout, ep_in = testepin, maxpkt=512;
#ifdef DEBUG_UMSC
  uint last_lba, sector_size;
#endif
  static int counter = 0;
  {
    if(counter++ < 5) {
      uint8 cmd[16] = {0x25,0,0,0,0,0,0,0,0,0,0,0};
      DLOG ("SENDING READ CAPACITY");
      DLOG("testinfo = 0x%p", testinfo);
      umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 0x8, maxpkt);
#ifdef DEBUG_UMSC
      last_lba = conf[3] | conf[2] << 8 | conf[1] << 16 | conf[0] << 24;
      sector_size = conf[7] | conf[6] << 8 | conf[5] << 16 | conf[4] << 24;
#endif
      DLOG ("sector_size=0x%x last_lba=0x%x total_size=%d bytes",
            sector_size, last_lba, sector_size * (last_lba + 1));
    }
  }
  #endif
}

static int umsc_read(USB_DEVICE_INFO* device, int dev_num, char* buf, int data_len)
{
  DLOG("umsc_read called");
  umsc_tmr_test ();
  return -1;
}

static int umsc_write(USB_DEVICE_INFO* device, int dev_num, char* buf, int data_len)
{
  umsc_tmr_test ();
  return -1;
}

static bool
umsc_probe (USB_DEVICE_INFO *info, USB_CFG_DESC *cfgd, USB_IF_DESC *ifd);

static USB_DRIVER umsc_driver = {
  .probe = umsc_probe,
  .read = umsc_read,
  .write = umsc_write
};

static bool
umsc_probe (USB_DEVICE_INFO *info, USB_CFG_DESC *cfgd, USB_IF_DESC *ifd)
{
  uint i, ep_in=0, ep_out=0;
#ifdef DEBUG_UMSC
  uint addr = info->address;
#endif
  USB_EPT_DESC *ep;
  uint last_lba, sector_size, maxpkt=64;
  uint8 conf[512];
  umsc_device_t *umsc;

  if (num_umsc_devs >= UMSC_MAX_DEVICES)
    return FALSE;

  if (ifd->bInterfaceClass != USB_MASS_STORAGE_CLASS ||
      ifd->bInterfaceProtocol != UMSC_PROTOCOL)
    return FALSE;

  ep = (USB_EPT_DESC *)(&ifd[1]);


  if( (ep[0].wMaxPacketSize != ep[1].wMaxPacketSize) ||
      (ep[0].bDescriptorType != USB_TYPE_EPT_DESC) ||
      (ep[1].bDescriptorType != USB_TYPE_EPT_DESC) ) {
    DLOG ("endpoint packet sizes don't match! or descriptors aren't endpoint descriptors");
    return FALSE;
  }

  
  usb_register_device(info, &umsc_driver, "umsc");
  
  DLOG("endpoint packet size = %d", ep[0].wMaxPacketSize);
  maxpkt = ep[0].wMaxPacketSize;
  //maxpkt = 64;
  for (i=0; i<ifd->bNumEndpoints; i++)
    if (ep[i].bEndpointAddress & 0x80)
      ep_in = ep[i].bEndpointAddress & 0x7F;
    else
      ep_out = ep[i].bEndpointAddress & 0x7F;

  if (!(ep_in && ep_out))
    return FALSE;

  
  testinfo = info;
  testepin = ep_in;
  testepout = ep_out;

  DLOG("test info 0x%p\ntestepin %d\ntestepout %d", testinfo, testepin, testepout);

  DLOG ("detected device=%d ep_in=%d ep_out=%d maxpkt=%d",
        addr, ep_in, ep_out, maxpkt);

  info->ep_in[ep_in].desc.wMaxPacketSize = maxpkt;
  info->ep_out[ep_out].desc.wMaxPacketSize = maxpkt;

  usb_set_configuration (info, cfgd->bConfigurationValue);
  delay (50);

  

  {
    uint8 cmd[16] = {0x12,0,0,0,0x24,0,0,0,0,0,0,0};
    DLOG ("SENDING INQUIRY");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 0x24, maxpkt) < 0)
      return FALSE;
  }

  {
    uint8 cmd[16] = {0,0,0,0,0,0,0,0,0,0,0,0};
    DLOG ("SENDING TEST UNIT READY");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 0, maxpkt) < 0)
      return FALSE;
  }

  {
    uint8 cmd[16] = {0x03,0,0,0,0x24,0,0,0,0,0,0,0};
    DLOG ("SENDING REQUEST SENSE");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 28, maxpkt) < 0)
      return FALSE;
  }

  {
    uint8 cmd[16] = {0,0,0,0,0,0,0,0,0,0,0,0};
    DLOG ("SENDING TEST UNIT READY");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 0, maxpkt) < 0)
      return FALSE;
  }

  {
    uint8 cmd[16] = {0x03,0,0,0,0x24,0,0,0,0,0,0,0};
    DLOG ("SENDING REQUEST SENSE");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 28, maxpkt) < 0)
      return FALSE;
  }

  {
    uint8 cmd[16] = {0x25,0,0,0,0,0,0,0,0,0,0,0};
    DLOG ("SENDING READ CAPACITY");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 0x8, maxpkt) < 0)
      return FALSE;
    last_lba = conf[3] | conf[2] << 8 | conf[1] << 16 | conf[0] << 24;
    sector_size = conf[7] | conf[6] << 8 | conf[5] << 16 | conf[4] << 24;
    DLOG ("sector_size=0x%x last_lba=0x%x total_size=%d bytes",
            sector_size, last_lba, sector_size * (last_lba + 1));
  }

  memset (conf, 0, 512);
  {
    uint8 cmd[16] = { [0] = 0x28, [8] = 1 };
    DLOG ("SENDING READ (10)");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 512, maxpkt) < 0)
      return FALSE;
    DLOG ("read from sector 0: %.02X %.02X %.02X %.02X",
          conf[0], conf[1], conf[2], conf[3]);
    if (conf[510] == 0x55 && conf[511] == 0xAA)
      DLOG ("Found boot sector magic number");
  }

  umsc = &umsc_devs[num_umsc_devs];
  umsc->devinfo = info;
  umsc->sector_size = sector_size;
  umsc->last_lba = last_lba;
  umsc->ep_out = ep_out;
  umsc->ep_in = ep_in;
  umsc->maxpkt = maxpkt;

  DLOG ("Registered UMSC device index=%d", num_umsc_devs);

  num_umsc_devs++;
  
  {
    uint8 cmd[16] = {0x25,0,0,0,0,0,0,0,0,0,0,0};
    DLOG ("SENDING READ CAPACITY");
    if (umsc_bulk_scsi (info, ep_out, ep_in, cmd, 1, conf, 0x8, maxpkt) < 0)
      return FALSE;
    last_lba = conf[3] | conf[2] << 8 | conf[1] << 16 | conf[0] << 24;
    sector_size = conf[7] | conf[6] << 8 | conf[5] << 16 | conf[4] << 24;
    DLOG ("sector_size=0x%x last_lba=0x%x total_size=%d bytes",
          sector_size, last_lba, sector_size * (last_lba + 1));
  }
  
  umsc_tmr_test();
  
  return TRUE;
}





extern bool
usb_mass_storage_driver_init (void)
{
  return usb_register_driver (&umsc_driver);
}





#include "module/header.h"

static const struct module_ops mod_ops = {
  .init = usb_mass_storage_driver_init
};

DEF_MODULE (usb___umsc, "USB mass storage driver", &mod_ops, {"usb"});


/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
