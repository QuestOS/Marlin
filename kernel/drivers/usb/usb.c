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

#include <drivers/usb/ehci.h>
#include <drivers/usb/usb.h>
#include <arch/i386.h>
#include <util/printf.h>
#include <kernel.h>
#include "sched/sched.h"
#include <mem/malloc.h>

#define DEBUG_USB

#ifdef DEBUG_USB
#define DLOG(fmt,...) DLOG_PREFIX("USB", fmt, ##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

#define MAX_USB_HCD 10
usb_hcd_t* usb_hcd_list[MAX_USB_HCD];
uint32_t usb_hcd_list_next = 0;

#define USB_MAX_DEVICE_DRIVERS 32
static USB_DRIVER drivers[USB_MAX_DEVICE_DRIVERS];
static uint num_drivers = 0;


typedef struct
{
  char* name;
  USB_DEVICE_INFO* usb_device;
} usb_registered_device_t;

#define USB_CORE_MAX_DEVICES 32
#define USB_CORE_MAX_DEVICES_LOG_10 2
static usb_registered_device_t devices[USB_CORE_MAX_DEVICES];
static uint num_devices = 0;


/* -- EM -- Both EHCI and net2280 use this so I am putting it here for
      now */
bool
handshake(uint32_t *ptr, uint32_t mask, uint32_t done, uint32_t usec)
{
  uint32_t result;
  
  do {
    result = *ptr;
    if (result == ~(uint32_t)0) return FALSE; /* card removed */
    result &= mask;
    if (result == done) return TRUE;
    tsc_delay_usec(1);
    usec--;
  } while (usec);
  
  return FALSE;
}

int usb_syscall_handler(uint32_t device_id, uint32_t operation,
                        char* buf, uint32_t data_len)
{
  int result;
  USB_DEVICE_INFO* device;
  
  
  device = usb_get_device(device_id);

  
  if(device == NULL) {
    DLOG("Unknown device id");
    return -1;
  }

  if(device->driver == NULL) return -1;
  
  switch(operation) {
  case USB_USER_READ:
    result = device->driver->read(device, device_id, buf, data_len);
    break;
    
  case USB_USER_WRITE:
    
    result = device->driver->write(device, device_id, buf, data_len);
    break;

  case USB_USER_OPEN:
    result = device->driver->open(device, device_id);
    break;
    
  case USB_USER_CLOSE:
    result = device->driver->close(device, device_id);
    break;

  default:
    DLOG("Unknown usb user operation %d", operation);
    return -1;
  }

  
  return result;
}

bool
initialise_usb_hcd(usb_hcd_t* usb_hcd, uint32_t usb_hc_type,
                   usb_reset_root_ports_func reset_root_ports,
                   usb_post_enumeration_func post_enumeration,
                   usb_submit_urb_func submit_urb,
                   usb_kill_urb_func kill_urb,
                   usb_rt_iso_update_packets_func rt_iso_update_packets,
                   usb_rt_iso_free_packets_func rt_iso_free_packets,
                   usb_rt_data_lost_func rt_data_lost,
                   usb_rt_update_data_func rt_update_data,
                   usb_rt_free_data_func rt_free_data,
                   usb_rt_push_data_func rt_push_data,
                   usb_rt_free_write_resources_func rt_free_write_resources)
{
  usb_hcd->usb_hc_type = usb_hc_type;
  usb_hcd->reset_root_ports      = reset_root_ports;
  usb_hcd->post_enumeration      = post_enumeration;
  
  usb_hcd->submit_urb            = submit_urb;
  usb_hcd->kill_urb              = kill_urb;
  
  usb_hcd->rt_iso_update_packets = rt_iso_update_packets;
  usb_hcd->rt_iso_free_packets   = rt_iso_free_packets;
  
  usb_hcd->rt_data_lost            = rt_data_lost;
  usb_hcd->rt_update_data          = rt_update_data;
  usb_hcd->rt_free_data            = rt_free_data;
  usb_hcd->rt_push_data            = rt_push_data;
  usb_hcd->rt_free_write_resources = rt_free_write_resources;
  
  
  usb_hcd->next_address = 1;
  return TRUE;
}

bool
add_usb_hcd(usb_hcd_t* usb_hcd)
{
  if(usb_hcd_list_next < MAX_USB_HCD) {
    usb_hcd_list[usb_hcd_list_next++] = usb_hcd;
    return TRUE;
  }
  return FALSE;
}

bool get_usb_device_id(char* name)
{
  int i;
  DLOG("in %s, name = %s, num_devices = %d", __FUNCTION__, name, num_devices);
  for(i = 0; i < num_devices; ++i) {
    DLOG("devices[%d]->name = %s", i, devices[i].name);
    if(!strcmp(devices[i].name, name)) return i;
  }
  DLOG("Failed to find device id");
  return -1;
}


static char* get_next_usb_device_name(char* base_name)
{
  uint i;
  uint base_name_len;
  uint possible_name_max_len;
  char *possible_name;

  if(base_name == NULL) return NULL;
  base_name_len = strlen(base_name);
  possible_name_max_len = base_name_len + 1 + USB_CORE_MAX_DEVICES_LOG_10;
  
  possible_name = kmalloc(possible_name_max_len);
  memcpy(possible_name, base_name, base_name_len);
  memset(&possible_name[base_name_len], 0, possible_name_max_len - base_name_len);
  for(i = 0; i < USB_CORE_MAX_DEVICES; ++i) {
    uint j;
    int_to_ascii(&possible_name[base_name_len], i);
    DLOG("possible name = %s", possible_name);
    for(j = 0; j < num_devices; ++j) {
      if(!strcmp(possible_name, devices[j].name)) break;
    }
    if(j == num_devices) return possible_name;
  }
  kfree(possible_name);
  return NULL;
}

int usb_register_device(USB_DEVICE_INFO* device, USB_DRIVER* driver, char* base_name)
{
  usb_registered_device_t* reg_dev = &devices[num_devices];
  if((reg_dev->name = get_next_usb_device_name(base_name)) == NULL) {
    panic("Failed to get device name");
    return -1;
  }
    
  if(num_devices < USB_CORE_MAX_DEVICES) {
    device->driver = driver;
    reg_dev->usb_device = device;
    num_devices++;
    return num_devices-1;
  }
  return -1;
}

USB_DEVICE_INFO* usb_get_device(int device_id)
{
  if(device_id < num_devices) return devices[device_id].usb_device;
  return NULL;
}

usb_hcd_t*
get_usb_hcd(uint32_t index)
{
  if(index < usb_hcd_list_next) return usb_hcd_list[index];
  return NULL;
}

/*
 * -- EM -- Currently mem_flags does not do anything, just have it
 * here for now for portability with Linux
 */
int usb_submit_urb (struct urb *urb, gfp_t mem_flags)
{
  if(mem_flags != 0) {
    /*
     * -- EM -- mem_flags can only be zero for right now.  Panic if it
     * is not to avoid when we copy over drivers that start using the
     * full functionality of usb_submit_urb
     */
    DLOG("mem_flags can only be zero for right now");
    panic("mem_flags != 0 in usb_submit_urb");
  }
  if(urb->realtime) {
    if(!mp_enabled) {
      DLOG("Can only submit realtime urbs when interrupts are on");
      return -1;
    }
    if((!usb_pipein(urb->pipe) && urb->context)) {
      DLOG("Cannot have context for real-time output pipes, context is specified at push");
      return -1;
    }
  }
  urb->active = TRUE;
  return urb->dev->hcd->submit_urb(urb, mem_flags);
}

/*
 * -- EM -- This function just calls the appropriate HCD kill_urb
 * function, it would be better if this some of this was abstracted
 * out into the core but for now it is all host controller specific
 */
void usb_kill_urb(struct urb *urb)
{
  if(urb->active) {
    urb->dev->hcd->kill_urb(urb);
  }
}



struct urb *usb_alloc_urb(int iso_packets, gfp_t mem_flags)
{
  struct urb *urb = kmalloc(sizeof(struct urb) +
                            iso_packets * sizeof(struct usb_iso_packet_descriptor));
  if (!urb) {
    DLOG("Failed to allocate urb");
    return NULL;
  }
  usb_init_urb(urb);
  return urb;
}

int usb_rt_iso_update_packets(struct urb* urb, int max_packets)
{
  return urb->dev->hcd->rt_iso_update_packets(urb, max_packets);
}

int usb_rt_iso_free_packets(struct urb* urb, int number_of_packets)
{
  return urb->dev->hcd->rt_iso_free_packets(urb, number_of_packets);
}

int usb_rt_data_lost(struct urb* urb)
{
  return urb->dev->hcd->rt_data_lost(urb);
}

int usb_rt_free_data(struct urb* urb, int count)
{
  return urb->dev->hcd->rt_free_data(urb, count);
}

int usb_rt_update_data(struct urb* urb, int max_count)
{
  return urb->dev->hcd->rt_update_data(urb, max_count);
}

int usb_rt_push_data(struct urb* urb, char* data, int count, uint interrupt_rate,
                     uint flags, void* context)
{
  return urb->dev->hcd->rt_push_data(urb, data, count, interrupt_rate, flags, context);
}

int usb_rt_free_write_resources(struct urb* urb)
{
  return urb->dev->hcd->rt_free_write_resources(urb);
}

static void usb_core_blocking_completion(struct urb* urb)
{
  *((bool*)urb->context) = TRUE;
}

int usb_isochronous_msg(struct usb_device *dev, unsigned int pipe,
                        void* data, int packet_size, int num_packets,
                        unsigned int* actual_lens, int* statuses,
                        int timeout)
{
  /*
   * -- EM -- It should be okay to put these on the stack because this
   * function won't complete until the callback is called
   */
  
  struct urb* urb = usb_alloc_urb(num_packets, 0);
  bool *done = kmalloc(sizeof(*done));

  int i;
  int ret;
  usb_complete_t complete_callback;
  struct usb_host_endpoint *ep;

  if(!urb || !done) {
    if(urb) usb_free_urb(urb);
    if(done) kfree(done);
    return -1;
  }
  
  ep = usb_pipe_endpoint(dev, pipe);
  
  memset(actual_lens, 0, sizeof(*actual_lens) * num_packets);
  memset(statuses,    0, sizeof(*statuses)    * num_packets);
    
  if(mp_enabled) {
    complete_callback = usb_core_blocking_completion;
  }
  else {
    complete_callback  = NULL;
    urb->timeout = timeout;
  }
  
  usb_fill_iso_urb(urb, dev, pipe, data, complete_callback, done,
                   ep->desc.bInterval, num_packets, packet_size);
  
  ret = usb_submit_urb(urb, 0);
  
  if(ret < 0) goto usb_isochronous_msg_out;
  
  if(mp_enabled) {
    
    /*
     * -- EM -- So the best way to do this would have the processes
     * sleep the entire time until either woken up or a timeout
     * occurred.  Quest does not have this functionality yet (or maybe
     * it does and I just don't know) so just going to do a spin/sleep
     * combo looking at a boolean to see if the completion has
     * finished
     */
    
    /*
     * -- EM -- Timeout is specified in jiffies, right now assuming
     * the Linux default of 4ms
     */
    
    /* add one for integer rounding*/
    
    for(i = 0; (!(*done)) && (i < (timeout / USB_MSG_SLEEP_INTERVAL) + 1 ); ++i) {
      sched_usleep(4000 * USB_MSG_SLEEP_INTERVAL);
    }
    
    ret = (*done) ? 0 : -1;
  }

 usb_isochronous_msg_out:
  
  for(i = 0; i < num_packets; ++i) {
    actual_lens[i] = urb->iso_frame_desc[i].actual_length;
    statuses[i] = urb->iso_frame_desc[i].status;
  }
  usb_free_urb(urb);
  kfree(done);
  return ret;
}

int usb_interrupt_msg(struct usb_device *usb_dev, unsigned int pipe,
                      void *data, int len, int *actual_length,
                      int timeout)
{
  /*
   * -- EM -- It should be okay to put these on the stack because this
   * function won't complete until the callback is called
   */

  struct urb* urb = usb_alloc_urb(0, 0);
  bool *done = kmalloc(sizeof(*done));
  int i;
  int ret;
  usb_complete_t complete_callback;
  struct usb_host_endpoint *ep;
  if(!urb || !done) {
    if(urb) usb_free_urb(urb);
    if(done) kfree(done);
    return -1;
  }

  
  ep = usb_pipe_endpoint(usb_dev, pipe);

  if(mp_enabled) {
    complete_callback = usb_core_blocking_completion;
  }
  else {
    complete_callback  = NULL;
    urb->timeout = timeout;
  }

  usb_fill_int_urb(urb, usb_dev, pipe, data, len, complete_callback, done,
                   ep->desc.bInterval);
  urb->actual_length = 0;

  ret = usb_submit_urb(urb, 0);

  if(ret < 0) goto usb_interrupt_msg_out;
  
  if(mp_enabled) {

    /*
     * -- EM -- So the best way to do this would have the processes
     * sleep the entire time until either woken up or a timeout
     * occurred.  Quest does not have this functionality yet (or maybe
     * it does and I just don't know) so just going to do a spin/sleep
     * combo looking at a boolean to see if the completion has
     * finished
     */
    
    /*
     * -- EM -- Timeout is specified in jiffies, right now assuming
     * the Linux default of 4ms
     */

    /* add one for integer rounding*/
    
    for(i = 0; (!(*done)) && (i < (timeout / USB_MSG_SLEEP_INTERVAL) + 1 ); ++i) {
      sched_usleep(4000 * USB_MSG_SLEEP_INTERVAL);
      
    }

    ret = (*done) ? 0 : -1;

  }
  
 usb_interrupt_msg_out:

  *actual_length = urb->actual_length;
  /* Must free hcpriv since we are not allocating the urb via
     usb_alloc_urb */
  usb_free_urb(urb);
  kfree(done);
  return ret;

}

int usb_bulk_msg(struct usb_device *usb_dev, unsigned int pipe,
                 void *data, int len, int *actual_length,
                 int timeout)
{
  /*
   * -- EM -- It should be okay to put these on the stack because this
   * function won't complete until the callback is called
   */
  struct urb* urb = usb_alloc_urb(0, 0);
  bool *done = kmalloc(sizeof(*done));
  int i;
  int ret;
  usb_complete_t complete_callback;
  if(!urb || !done) {
    if(urb) usb_free_urb(urb);
    if(done) kfree(done);
    return -1;
  }

  *done = FALSE;

  
  if(mp_enabled) {
    complete_callback = usb_core_blocking_completion;
  }
  else {
    complete_callback  = NULL;
    urb->timeout = timeout;
  }

  usb_fill_bulk_urb(urb, usb_dev, pipe, data, len, complete_callback, done);
  urb->actual_length = 0;

  ret = usb_submit_urb(urb, 0);

  if(ret < 0) goto usb_bulk_msg_out;
  
  if(mp_enabled) {

    /*
     * -- EM -- So the best way to do this would have the processes
     * sleep the entire time until either woken up or a timeout
     * occurred.  Quest does not have this functionality yet (or maybe
     * it does and I just don't know) so just going to do a spin/sleep
     * combo looking at a boolean to see if the completion has
     * finished
     */

    /*
     * -- EM -- Timeout is specified in jiffies, right now assuming
     * the Linux default of 4ms
     */
    
    /* add one for integer rounding*/
    
    for(i = 0; (!(*done)) && (i < (timeout / USB_MSG_SLEEP_INTERVAL) + 1 ); ++i) {
      sched_usleep(4000 * USB_MSG_SLEEP_INTERVAL);
      
    }

    ret = (*done) ? 0 : -1;

  }
  
 usb_bulk_msg_out:

  *actual_length = urb->actual_length;
  /* Must free hcpriv since we are not allocating the urb via
     usb_alloc_urb */
  usb_free_urb(urb); 
  kfree(done);
  return ret;
}


int usb_control_msg(struct usb_device *dev, unsigned int pipe,
                    uint8_t request, uint8_t requesttype,
                    uint16_t value, uint16_t index,
                    void *data, uint16_t size, int timeout)
{
  /*
   * -- EM -- It should be okay to put these on the stack because this
   * function won't complete until the callback is called,
   */
  /* -- EM -- not true if it fails! */
  
  struct urb* urb = usb_alloc_urb(0, 0);
  bool* done = kmalloc(sizeof(*done));
  int i;
  USB_DEV_REQ cmd;
  int ret;
  usb_complete_t complete_callback;
  if(!urb || !done) {
    if(urb) usb_free_urb(urb);
    if(done) kfree(done);
    return -1;
  }

  *done = FALSE;

  usb_init_urb(urb);
  cmd.bmRequestType = requesttype;
  cmd.bRequest = request;
  cmd.wValue = cpu_to_le16(value);
  cmd.wIndex = cpu_to_le16(index);
  cmd.wLength = cpu_to_le16(size);
  
  if(mp_enabled) {
    complete_callback = usb_core_blocking_completion;
  }
  else {
    complete_callback = NULL;
    urb->timeout = timeout;
  }
  
  usb_fill_control_urb(urb, dev, pipe, (unsigned char *)&cmd, data,
                       size, complete_callback, done);
  
  ret = usb_submit_urb(urb, 0);

  if(ret < 0) goto usb_control_msg_out;

  if(mp_enabled) {

    /*
     * -- EM -- So the best way to do this would have the processes
     * sleep the entire time until either woken up or a timeout
     * occurred.  Quest does not have this functionality yet (or maybe
     * it does and I just don't know) so just going to do a spin/sleep
     * combo looking at a boolean to see if the completion has
     * finished
     */

    /*
     * -- EM -- Timeout is specified in jiffies, right now assuming
     * the Linux default of 4ms
     */
    
    /* add one for integer rounding*/
    
    for(i = 0; (!(*done)) && (i < (timeout / USB_MSG_SLEEP_INTERVAL) + 1 ); ++i) {
      sched_usleep(4000 * USB_MSG_SLEEP_INTERVAL);
    }
    if(!(*done)) {
      DLOG("Failed to complete callback for control msg");
      int* temp = 0;
      *temp = 1;
      panic("Failed to complete callback for control msg");
    }
    ret = *done ? 0 : -1;
  }
  
 usb_control_msg_out:
  usb_free_urb(urb);
  kfree(done);
  return ret;  
}

int
usb_get_descriptor(USB_DEVICE_INFO* dev,
                   uint16_t dtype,
                   uint16_t dindex,
                   uint16_t index,
                   uint16_t length,
                   addr_t desc)
{
  return usb_control_msg(dev, usb_rcvctrlpipe(dev,0), USB_GET_DESCRIPTOR,
                         USB_DIR_IN, (dtype << 8) + dindex, index, desc, length,
                         USB_DEFAULT_CONTROL_MSG_TIMEOUT);
}


int
usb_set_address (USB_DEVICE_INFO* dev, uint8_t new_addr)
{
  sint status;


  status = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), USB_SET_ADDRESS, USB_DIR_OUT,
                           new_addr, 0, NULL, 0, USB_DEFAULT_CONTROL_MSG_TIMEOUT);
  
  if (status >= 0) {
    dev->endpoint_toggles = 0;
  }
  return status;
  
}

int
usb_get_configuration(USB_DEVICE_INFO* dev)
{
  switch (dev->host_type)
    {
    case USB_TYPE_HC_UHCI :
      panic("UHCI broken: usb_get_configuration");
      /*
      if ((dev->devd).bMaxPacketSize0 == 0) {
        DLOG("USB_DEVICE_INFO is probably not initialized!");
        return -1;
      } else {
        return uhci_get_configuration(dev->address,
                                      (dev->devd).bMaxPacketSize0);
      }
      */

    case USB_TYPE_HC_EHCI :
      DLOG("EHCI Host Controller is not supported now! usb_get_configuration");
      panic("EHCI Host Controller is not supported now! usb_get_configuration");
      return -1;

    case USB_TYPE_HC_OHCI :
      DLOG("OHCI Host Controller is not supported now!");
      return -1;

    default :
      DLOG("Unknown Host Controller request!");
      return -1;
    }
  return -1;
  
}

int
usb_set_configuration(USB_DEVICE_INFO * dev, uint8_t conf)
{
  return usb_control_msg(dev, usb_sndctrlpipe(dev, 0), USB_SET_CONFIGURATION,
                         USB_DIR_OUT, conf, 0, NULL, 0,
                         USB_DEFAULT_CONTROL_MSG_TIMEOUT);
}

int
usb_get_interface(USB_DEVICE_INFO * dev, uint16_t interface)
{
  switch (dev->host_type)
    {
    case USB_TYPE_HC_UHCI :
      panic("UHCI broken: usb_get_interface");
      /*
      if ((dev->devd).bMaxPacketSize0 == 0) {
        DLOG("USB_DEVICE_INFO is probably not initialized!");
        return -1;
      } else {
        return uhci_get_interface(dev->address, interface,
                                  (dev->devd).bMaxPacketSize0);
      }
      */

    case USB_TYPE_HC_EHCI :
      DLOG("EHCI Host Controller is not supported now! usb_get_interface");
      panic("ehci usb_get_interface not supported");
      return -1;

    case USB_TYPE_HC_OHCI :
      DLOG("OHCI Host Controller is not supported now!");
      return -1;

    default :
      DLOG("Unknown Host Controller request!");
      return -1;
    }
  return -1;
}

int
usb_set_interface(USB_DEVICE_INFO * dev, uint16_t alt, uint16_t interface)
{
  return usb_control_msg(dev, usb_sndctrlpipe(dev, 0), USB_SET_INTERFACE,
                         0x01, alt, interface, NULL, 0,
                         USB_DEFAULT_CONTROL_MSG_TIMEOUT);
}


bool
usb_register_driver (USB_DRIVER *driver)
{
  if (num_drivers >= USB_MAX_DEVICE_DRIVERS) return FALSE;
  memcpy (&drivers[num_drivers], driver, sizeof (USB_DRIVER));
  num_drivers++;
  return TRUE;
}

static void
dlog_devd (USB_DEV_DESC *devd)
{
  DLOG ("DEVICE DESCRIPTOR len=%d type=%d bcdUSB=0x%x",
        devd->bLength, devd->bDescriptorType, devd->bcdUSB);
  DLOG ("  class=0x%x subclass=0x%x proto=0x%x maxpkt0=%d",
        devd->bDeviceClass, devd->bDeviceSubClass, devd->bDeviceProtocol,
        devd->bMaxPacketSize0);
  DLOG ("  vendor=0x%x product=0x%x bcdDevice=0x%x numcfgs=%d",
        devd->idVendor, devd->idProduct, devd->bcdDevice,
        devd->bNumConfigurations);
}

static void
dlog_info (USB_DEVICE_INFO *info)
{
  DLOG ("ADDRESS %d", info->address);
  dlog_devd (&info->devd);

#if 0
  uint8 strbuf[64];
  uint8 str[32];
#define do_str(slot,label)                                              \
  if (info->devd.slot != 0 &&                                           \
      uhci_get_string (info->address, info->devd.slot, 0,               \
                       sizeof (strbuf), strbuf,                         \
                       info->devd.bMaxPacketSize0)                      \
      == 0) {                                                           \
    memset (str, 0, sizeof (str));                                      \
    if (uhci_interp_string ((USB_STR_DESC *)strbuf, sizeof (strbuf), 0, \
                            str, sizeof (str)-1) > 0) {                 \
      DLOG ("  "label": %s", str);                                      \
    }                                                                   \
  }

  do_str (iManufacturer, "Manufacturer");
  do_str (iProduct, "Product");
#undef do_str
#endif
}

static void
find_device_driver (USB_DEVICE_INFO *info, USB_CFG_DESC *cfgd, USB_IF_DESC *ifd)
{
  int d;
  dlog_info(info);
  DLOG("In %s", __FUNCTION__);
  for (d=0; d<num_drivers; d++) {
    if (drivers[d].probe (info, cfgd, ifd)) return;
  }
  DLOG("Unknown device bDeviceClass = 0x%X\n\tbDeviceSubClass = 0x%X"
       "\n\tVendor = 0x%X Product = 0x%X\n\tfile %s line %d",
       info->devd.bDeviceClass, info->devd.bDeviceSubClass,
       info->devd.idVendor, info->devd.idProduct,
       __FILE__, __LINE__);
  //panic("Unknown Device");
}

void print_all_descriptors(void* descriptor_start, uint total_length)
{
  struct usb_descriptor_header* temp = descriptor_start;
  
  uint total_traversed = 0;
  
  while(total_traversed < total_length) {
    DLOG("Descriptor Type = 0x%X", temp->bDescriptorType);
#define PRINT_DESCRIPTOR_SWITCH_CASE(desc)      \
    case desc:                                  \
      DLOG("\t"#desc);                          \
      break
      
    switch(temp->bDescriptorType) {
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_DEV_DESC);
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_CFG_DESC);
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_STR_DESC);
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_IF_DESC);
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_EPT_DESC);
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_QUA_DESC);
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_SPD_CFG_DESC);
      PRINT_DESCRIPTOR_SWITCH_CASE(USB_TYPE_IF_PWR_DESC);

#undef PRINT_DESCRIPTOR_SWITCH_CASE
    default:
      DLOG("\tUnknown Type");
    }
    if(temp->bLength == 0) return;
    
    total_traversed += temp->bLength;
    temp = (struct usb_descriptor_header*)(((char*)temp) + temp->bLength);
  }
}

void* get_next_desc(uint desc_type, void* descriptor_start, uint remaining_length)
{
  
  struct usb_descriptor_header* temp = descriptor_start;
  
  uint total_traversed = 0;

  while(total_traversed < remaining_length) {
    if(temp->bDescriptorType == desc_type) {
      return temp;
    }

    total_traversed += temp->bLength;
    temp = (struct usb_descriptor_header*)(((char*)temp) + temp->bLength);
  }
  return NULL;
}

void print_ept_desc_info(USB_EPT_DESC* ept_desc)
{
  DLOG("Endpoint Address = 0x%X", ept_desc->bEndpointAddress);
  DLOG("Attributes = 0x%X", ept_desc->bmAttributes);
  DLOG("wMaxPacketSize = 0x%X", ept_desc->wMaxPacketSize);
  DLOG("Interval = 0x%X", ept_desc->bInterval);
}

void print_if_desc_info(USB_IF_DESC *ifd)
{
#define PRINT_IF_DESC_ELEMENT(e) DLOG("ifd->"#e" = 0x%X", ifd->e);
  DLOG("Interface descriptor information");
  PRINT_IF_DESC_ELEMENT(bLength);
  PRINT_IF_DESC_ELEMENT(bDescriptorType);
  PRINT_IF_DESC_ELEMENT(bInterfaceNumber);
  PRINT_IF_DESC_ELEMENT(bAlternateSetting);
  PRINT_IF_DESC_ELEMENT(bNumEndpoints);
  PRINT_IF_DESC_ELEMENT(bInterfaceClass);
  PRINT_IF_DESC_ELEMENT(bInterfaceSubClass);
  PRINT_IF_DESC_ELEMENT(bInterfaceProtocol);
  PRINT_IF_DESC_ELEMENT(iInterface);

#undef PRINT_IF_DESC_ELEMENT
}

/* figures out what device is attached as address 0 */
bool
usb_enumerate(usb_hcd_t* usb_hcd, uint dev_speed, uint hub_addr, uint port_num)
{
  USB_DEV_DESC devd;
  USB_CFG_DESC *cfgd;
  USB_IF_DESC *ifd;
#define TEMPSZ 256
  uint8 temp[TEMPSZ], *ptr;
  sint status, c, i, total_length=0;
  USB_DEVICE_INFO *info;
  uint curdev = usb_hcd->next_address;

  if (curdev > USB_MAX_DEVICES) {
    DLOG ("usb_enumerate: too many devices");
    return FALSE;
  }

  DLOG ("usb_enumerate: curdev=%d", curdev);

  /* clear device info */
  info = &usb_hcd->devinfo[curdev];
  memset (info, 0, sizeof (USB_DEVICE_INFO));
  info->address = 0;
  info->host_type = usb_hcd->usb_hc_type;
  info->hcd = usb_hcd;
  info->endpoint_toggles = 0;
  info->speed = dev_speed;
  info->hub_addr = hub_addr;
  info->port_num = port_num;
  DLOG("dev_speed = %u", dev_speed);
  switch(dev_speed) {
  case USB_SPEED_LOW:
    info->devd.bMaxPacketSize0
      = info->ep_in [0].desc.wMaxPacketSize
      = info->ep_out[0].desc.wMaxPacketSize = 8;
    break;

  case USB_SPEED_HIGH:
  case USB_SPEED_FULL:
    info->devd.bMaxPacketSize0
      = info->ep_in [0].desc.wMaxPacketSize
      = info->ep_out[0].desc.wMaxPacketSize = 64;
    break;

  default:
    DLOG("Unknown device speed passed to %s", __FUNCTION__);
    return FALSE;
  }
  
  memset (&devd, 0, sizeof (USB_DEV_DESC));

  /* get device descriptor */
  status = usb_get_descriptor (info, USB_TYPE_DEV_DESC, 0, 0,
                               sizeof (USB_DEV_DESC), &devd);
  if (status < 0) {
    DLOG("Could not get descriptor");
    goto abort;
  }
  
  dlog_devd(&devd);

  /* -- EM -- I don't think the below is necessary anymore since we
     pass the device speed to usb_enumerate so I am disabling it */
#if 0
  if (devd.bMaxPacketSize0 == 8) {
    /* get device descriptor */
    info->devd.bMaxPacketSize0 = 8;
    status = usb_get_descriptor(info, USB_TYPE_DEV_DESC, 0, 0,
                                sizeof(USB_DEV_DESC), &devd);
    if (status < 0)
      goto abort;
  }
#endif
  
  if (devd.bNumConfigurations == 255)
    devd.bNumConfigurations = 1; /* --!!-- hack */
  
  /* Update device info structure. Put it in USB core might be better */
  memcpy (&info->devd, &devd, sizeof (USB_DEV_DESC));
  
  /* assign an address */
  if (usb_set_address (info, curdev) < 0)
    goto abort;
  
  DLOG ("usb_enumerate: set (0x%x, 0x%x, 0x%x) to addr %d",
        devd.bDeviceClass, devd.idVendor, devd.idProduct, curdev);
  delay (2);
  
  DLOG ("usb_enumerate: num configs=%d", devd.bNumConfigurations);
  
  /* Update device info structure for new address. */
  info->address = curdev;



  for (c=0; c<devd.bNumConfigurations; c++) {
    /* get a config descriptor for size field */
    memset (temp, 0, TEMPSZ);
    status = usb_get_descriptor (info, USB_TYPE_CFG_DESC, c, 0,
                                 sizeof (USB_CFG_DESC), temp);
    if (status < 0) {
      DLOG ("usb_enumerate: failed to get config descriptor for c=%d", c);
      goto abort;
    }

    cfgd = (USB_CFG_DESC *)temp;
    DLOG ("usb_enumerate: c=%d cfgd->wTotalLength=%d", c, cfgd->wTotalLength);
    total_length += cfgd->wTotalLength;
  }

  DLOG ("usb_enumerate: total_length=%d", total_length);

  /* allocate memory to hold everything */
  info->configurations = kmalloc(total_length);
  if (!info->configurations) {
    DLOG ("usb_enumerate: kmalloc (%d) failed", total_length);
    goto abort;
  }

  /* read all cfg, if, and endpoint descriptors */
  ptr = info->configurations;
  for (c=0; c < devd.bNumConfigurations; c++) {
    /* obtain precise size info */
    memset (temp, 0, TEMPSZ);
    status = usb_get_descriptor (info, USB_TYPE_CFG_DESC, c, 0,
                                 sizeof (USB_CFG_DESC), temp);
    if (status < 0) {
      DLOG ("usb_enumerate: failed to get config descriptor for c=%d", c);
      goto abort_mem;
    }
    cfgd = (USB_CFG_DESC *)temp;

    /* get cfg, if, and endp descriptors */
    status =
      usb_get_descriptor (info, USB_TYPE_CFG_DESC, c, 0, cfgd->wTotalLength, ptr);
    if (status < 0)
      goto abort_mem;
    //print_all_descriptors(ptr, 1000);

    cfgd = (USB_CFG_DESC *)ptr;
    DLOG ("usb_enumerate: cfg %d has num_if=%d", c, cfgd->bNumInterfaces);
    ptr += cfgd->wTotalLength;
  }

  

  /* incr this here because hub drivers may recursively invoke enumerate */
  usb_hcd->next_address++;

  /* parse cfg and if descriptors */
  ptr = info->configurations;
  //print_all_descriptors(ptr, total_length);
  for (c=0; c < devd.bNumConfigurations; c++) {
    cfgd = (USB_CFG_DESC *) ptr;
    ptr += cfgd->bLength;
    for (i=0; i < cfgd->bNumInterfaces; i++) {
      /* find the next if descriptor, skipping any class-specific stuff */
      for (ifd = (USB_IF_DESC *) ptr;
           ifd->bDescriptorType != USB_TYPE_IF_DESC;
           ifd = (USB_IF_DESC *)((uint8 *)ifd + ifd->bLength)) {
        //DLOG ("ifd=%p len=%d type=0x%x", ifd, ifd->bLength, ifd->bDescriptorType);
        if(ifd->bLength == 0) {
          DLOG("Descriptor length is zero");
          panic("Descriptor length is zero");
        }
      }
      ptr = (uint8 *) ifd;
      DLOG ("usb_enumerate: examining (%d, %d) if_class=0x%X sub=0x%X proto=0x%X #endp=%d",
            c, i, ifd->bInterfaceClass,
            ifd->bInterfaceSubClass,
            ifd->bInterfaceProtocol,
            ifd->bNumEndpoints);

      /* find a device driver interested in this interface */
      find_device_driver (info, cfgd, ifd);

      ptr += ifd->bLength;
    }
    ptr = ((uint8 *)cfgd) + cfgd->wTotalLength;
  }

  /*
   * --??-- what happens if more than one driver matches more than one
   * config?
   */
  

  return TRUE;

 abort_mem:
  kfree(info->configurations);
 abort:
  return FALSE;
}

void dlog_usb_hcd(usb_hcd_t* usb_hcd)
{
  DLOG("usb_hc_type:  %d", usb_hcd->usb_hc_type);
  DLOG("next_address: %d", usb_hcd->next_address);
}


int usb_payload_size(USB_DEVICE_INFO* dev,
                     USB_EPT_DESC* endpoint)
{
  int wMaxPacketSize = endpoint->wMaxPacketSize;
  
  switch(dev->speed) {
  case USB_SPEED_LOW:
  case USB_SPEED_FULL:
    return wMaxPacketSize & 0x07FF;

  case USB_SPEED_HIGH:
    
    return (1 + (((wMaxPacketSize) >> 11) & 0x03)) * ((wMaxPacketSize) & 0x07ff);
  default:
    DLOG("Unhandled USB speed %d for %s", dev->speed, __FUNCTION__);
    return -1;
  }
}

const char *usb_speed_string(int speed)
{
  static const char *const names[] = {
    [USB_SPEED_UNKNOWN] = "UNKNOWN",
    [USB_SPEED_LOW] = "low-speed",
    [USB_SPEED_FULL] = "full-speed",
    [USB_SPEED_HIGH] = "high-speed",
    [USB_SPEED_WIRELESS] = "wireless",
    [USB_SPEED_SUPER] = "super-speed",
  };
  
  if (speed < 0 || speed >= ARRAY_SIZE(names))
    speed = USB_SPEED_UNKNOWN;
  
  return names[speed];

  #undef ARRAY_SIZE
}

bool
usb_init (void)
{
  return TRUE;
}

#include "module/header.h"

static const struct module_ops mod_ops = {
  .init = usb_init
};

DEF_MODULE (usb, "USB manager", &mod_ops, {});

/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
