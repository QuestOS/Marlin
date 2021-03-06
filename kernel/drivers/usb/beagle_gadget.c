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

#include <arch/i386.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/beagle_gadget.h>
#include <util/printf.h>
#include <kernel.h>
#include <sched/sched.h>
#include <arch/i386-div64.h>
#include <arch/i386.h>

#define DEBUG_GADGET
//#define DEBUG_GADGET_VERBOSE

#ifdef DEBUG_GADGET
#define DLOG(fmt,...) DLOG_PREFIX("beagle-gadget",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

#ifdef DEBUG_GADGET_VERBOSE
#define DLOGV(fmt,...) DLOG_PREFIX("beagle-gadget",fmt,##__VA_ARGS__)
#else
#define DLOGV(fmt, ...) ;
#endif

#define DLOG_INT(val) DLOG(#val " = %d", (int)(val));

#define min(x,y) ( x < y ? x : y)


static bool gadget_probe (USB_DEVICE_INFO *device, USB_CFG_DESC *cfg,
                          USB_IF_DESC *desc);

#define MAX_GADGET_DEVICES 5
static int current_gadget_dev_count = 0;
static gadget_device_info_t gadget_devices[MAX_GADGET_DEVICES];

static void initialise_gadget_dev_info(gadget_device_info_t* dev)
{ 
  memset(dev, 0, sizeof(*dev));
}

static void beagle_complete_callback(struct urb* urb)
{
  static uint64_t old_tsc = 0;
  static uint64_t current_tsc = 0;
  RDTSC(current_tsc);

  if(old_tsc != 0) {
    DLOG("Time Diff = 0x%X", current_tsc - old_tsc);
  }
  else {
    DLOG("FFF");
  }
  
  
  old_tsc = current_tsc;
}

static int gadget_open(USB_DEVICE_INFO* device, int dev_num)
{
  gadget_sub_device_info_t* gadget_dev = get_gadget_sub_dev_info(device, dev_num);
  struct usb_host_endpoint *ep = usb_pipe_endpoint(device, gadget_dev->pipe);
  int num_packets = (1024 * 8) >> (ep->desc.bInterval - 1);
  int result;
  uint64_t current_tsc;
  char* buf;
  
  gadget_dev->next_to_read = gadget_dev->data_available = 0;
  
  if(usb_pipetype(gadget_dev->pipe) == PIPE_ISOCHRONOUS) {
    buf = kmalloc(sizeof(usb_iso_packet_descriptor_t) * num_packets);
    if(buf == NULL) return -1;
    
    gadget_dev->urb = usb_alloc_urb(num_packets, 0);
    if(gadget_dev->urb == NULL) {
      kfree(buf);
      return -1;
    }
    
    gadget_dev->num_packets = num_packets;
    gadget_dev->buffer_size = sizeof(usb_iso_packet_descriptor_t) * num_packets;
  }
  else {
    buf = kmalloc(1024*200);
    if(buf == NULL) {
      return -1;
    }
    gadget_dev->buffer_size = 1024*200;
    gadget_dev->urb = usb_alloc_urb(0, 0);
    if(gadget_dev->urb == NULL) {
      DLOG("Failed to alloc URB");
      kfree(buf);
      return -1;
    }
    
  }

  //DLOG("dev_num = %d urb = 0x%p", dev_num, gadget_dev->urb);

  switch(usb_pipetype(gadget_dev->pipe)) {
  case PIPE_ISOCHRONOUS:
    usb_fill_rt_iso_urb(gadget_dev->urb, device, gadget_dev->pipe, buf,
                        beagle_complete_callback, NULL,
                        ep->desc.bInterval, num_packets,
                        gadget_dev->transaction_size, 2);
    
    DLOG("Init iso urb with interrupts");
    break;
  case PIPE_INTERRUPT:
    usb_fill_rt_int_urb(gadget_dev->urb, device, gadget_dev->pipe, buf, gadget_dev->buffer_size,
                        beagle_complete_callback, NULL,
                        ep->desc.bInterval, 512);
    break;
  case PIPE_BULK:
    usb_fill_rt_bulk_urb(gadget_dev->urb, device, gadget_dev->pipe, buf, gadget_dev->buffer_size,
                         beagle_complete_callback, NULL,
                         1, gadget_dev->buffer_size);
    break;
  default: // PIPE_CONTROL:
    DLOG("Gadget EP should never be control");
    panic("Gadget EP should never be control");
  }

  if((result = usb_submit_urb(gadget_dev->urb, 0)) < 0) {
    //DLOG("Failed to submit rt urb result = %d", result);
    return result;
  }
  RDTSC(current_tsc);
  gadget_dev->start_time = current_tsc;
  return 0;
}

static int gadget_close(USB_DEVICE_INFO* device, int dev_num)
{
  return -1;
}

static int gadget_read(USB_DEVICE_INFO* device, int dev_num, char* buf, int data_len)
{
#define BYTES_TO_READ (1024 * 800)
  
  gadget_sub_device_info_t* gadget_dev = get_gadget_sub_dev_info(device, dev_num);
  struct urb* urb = gadget_dev->urb;
  
  int bytes_freed;
  int bytes_to_copy = 0;

  if(!usb_pipein(gadget_dev->pipe)) {
    DLOG("Called read on an output endpoint/dev");
    return -1;
  }
  
  if(urb) {
    switch(usb_pipetype(gadget_dev->pipe)) {
    case PIPE_INTERRUPT:
      {
        int new_bytes = usb_rt_update_data(urb, BYTES_TO_READ);
        
        if(new_bytes < 0) {
          DLOG("new bytes < 0");
          panic("new bytes < 0");
        }
        if(new_bytes > 0) {
          gadget_dev->data_available += new_bytes;
          gadget_dev->total_bytes_read += new_bytes;
        }
        
        if(gadget_dev->data_available > 0) {
          
          bytes_to_copy = min(gadget_dev->data_available, data_len);
          DLOG_INT(gadget_dev->data_available);
          DLOG_INT(data_len);
          DLOG_INT(bytes_to_copy);


          if(gadget_dev->next_to_read + bytes_to_copy > gadget_dev->buffer_size) {
            int bytes_in_first_copy = gadget_dev->buffer_size - gadget_dev->next_to_read;
            memcpy(buf, gadget_dev->urb->transfer_buffer + gadget_dev->next_to_read,
                   bytes_in_first_copy);
            memcpy(buf + bytes_in_first_copy, gadget_dev->urb->transfer_buffer,
                    bytes_to_copy - bytes_in_first_copy);
          }
          else {
            memcpy(buf, gadget_dev->urb->transfer_buffer + gadget_dev->next_to_read, bytes_to_copy);
          }
          
          gadget_dev->next_to_read += bytes_to_copy;
          if(gadget_dev->next_to_read > gadget_dev->buffer_size) {
            gadget_dev->next_to_read -= gadget_dev->buffer_size;
          }
          bytes_freed = usb_rt_free_data(urb, bytes_to_copy);
          if(bytes_freed > 0) {
            gadget_dev->data_available -= bytes_freed;
          }
        }

        return bytes_to_copy;
      }

    case PIPE_ISOCHRONOUS:
      {
        int i;
        int bytes_filled_in = 0;
        char* urb_buffer_start = urb->transfer_buffer;
        usb_iso_packet_descriptor_t* packets = urb->iso_frame_desc;
        int new_packets = usb_rt_iso_update_packets(urb, gadget_dev->num_packets);
        uint packet_count_mask = urb->number_of_packets - 1;

        gadget_dev->data_available += new_packets;

        
        DLOG("%d: new packets = %d", dev_num, new_packets);

        for(i = 0; i < gadget_dev->data_available; ++i) {
          usb_iso_packet_descriptor_t* this_packet =
            &packets[(gadget_dev->next_to_read + i) & packet_count_mask];
          
          int new_bytes = this_packet->actual_length;
          if(new_bytes > (data_len - bytes_filled_in)) break;
          memcpy(&buf[bytes_filled_in], &urb_buffer_start[this_packet->offset], new_bytes);
          bytes_filled_in += new_bytes;
        }

        /* i is the number of packets we processed and need to free */

        gadget_dev->next_to_read = (gadget_dev->next_to_read + i) & packet_count_mask;

        if(i > 0) {
        
          int packets_freed = usb_rt_iso_free_packets(urb, i);
          if(packets_freed != i) {
            panic("Failed to free all packets");
            return -1;
          }
          if(packets_freed > 0) {
            gadget_dev->data_available -= packets_freed;
          }
        }

        return bytes_filled_in;
      }

    case PIPE_BULK:
      {
        int new_bytes = usb_rt_update_data(urb, BYTES_TO_READ);
        DLOG("new bytes = %d", new_bytes);
        if(new_bytes < 0) {
          DLOG("new bytes < 0");
          panic("new bytes < 0");
        }
        if(new_bytes > 0) {
          gadget_dev->data_available += new_bytes;
          gadget_dev->total_bytes_read += new_bytes;
        }
        
        if(gadget_dev->data_available > 0) {
          
          bytes_to_copy = min(gadget_dev->data_available, data_len);
          DLOG_INT(gadget_dev->data_available);
          DLOG_INT(data_len);
          DLOG_INT(bytes_to_copy);


          if(gadget_dev->next_to_read + bytes_to_copy > gadget_dev->buffer_size) {
            int bytes_in_first_copy = gadget_dev->buffer_size - gadget_dev->next_to_read;
            memcpy(buf, gadget_dev->urb->transfer_buffer + gadget_dev->next_to_read,
                   bytes_in_first_copy);
            memcpy(buf + bytes_in_first_copy, gadget_dev->urb->transfer_buffer,
                    bytes_to_copy - bytes_in_first_copy);
          }
          else {
            memcpy(buf, gadget_dev->urb->transfer_buffer + gadget_dev->next_to_read, bytes_to_copy);
          }
          
          gadget_dev->next_to_read += bytes_to_copy;
          if(gadget_dev->next_to_read > gadget_dev->buffer_size) {
            gadget_dev->next_to_read -= gadget_dev->buffer_size;
          }
          bytes_freed = usb_rt_free_data(urb, bytes_to_copy);
          if(bytes_freed > 0) {
            gadget_dev->data_available -= bytes_freed;
          }
        }
        return bytes_to_copy;
      }
    default: // case PIPE_CONTROL:
      {
        DLOG("Unsupported type in read");
        return -1;
      }
    }
  }
  else {
    DLOG("Urb not created need to call open first");
    return -1;
  }
}


static int gadget_write(USB_DEVICE_INFO* device, int dev_num, char* buf,
                        int data_len)
{
  uint64_t time;
  gadget_sub_device_info_t* gadget_dev = get_gadget_sub_dev_info(device, dev_num);
  struct urb* urb = gadget_dev->urb;
  int result;

  if(usb_pipein(gadget_dev->pipe)) {
    DLOG("Called write on an output endpoint/dev");
    return -1;
  }
  usb_rt_free_write_resources(gadget_dev->urb);
  
  if(urb) {
    switch(usb_pipetype(gadget_dev->pipe)) {
    case PIPE_INTERRUPT:
    case PIPE_BULK:
      return usb_rt_push_data(urb, buf, data_len, 512, 0, NULL);
            
    case PIPE_ISOCHRONOUS:
      return usb_rt_push_data(urb, buf, data_len, 2, 0, NULL);
        
    default: // case PIPE_CONTROL:
        DLOG("Unsupported pipe type");
        return -1;
    }
  }
  else {
    DLOG("Urb is not set call open first");
    return -1;
  }
}

static USB_DRIVER gadget_driver = {
  .probe = gadget_probe,
  .open  = gadget_open,
  .close = gadget_close,
  .read  = gadget_read,
  .write = gadget_write
};



static bool gadget_probe (USB_DEVICE_INFO *device, USB_CFG_DESC *cfg,
                          USB_IF_DESC *desc)
{
  static char temp[30];
  USB_IF_DESC* interface;
  USB_EPT_DESC* ept;
  gadget_device_info_t* gadget_dev;
  int i;
  int dev_num;
  DLOG("gadget_probe called");
  if(device->devd.idVendor != 0xabc4 ||
     device->devd.idProduct != 0xabc7) {
    return FALSE;
  }
  
  usb_set_configuration(device, cfg->bConfigurationValue);

  if(device->device_priv == NULL) {
    if(current_gadget_dev_count == MAX_GADGET_DEVICES) {
      DLOG("Too many gadget devices");
      return TRUE;
    }
    device->device_priv = &gadget_devices[current_gadget_dev_count++];
    initialise_gadget_dev_info(device->device_priv);
  }

  gadget_dev = get_gadget_dev_info(device);
  if(gadget_dev->initialised) {
    return TRUE;
  }

  gadget_dev->initialised = TRUE;
  
  DLOG("Descriptor total length = %d", cfg->wTotalLength);
  DLOG("Struct size is %d", sizeof(USB_CFG_DESC));
  DLOG("Number of interfaces = %d", cfg->bNumInterfaces);
  if(usb_get_descriptor(device, USB_TYPE_CFG_DESC, 0, 0, cfg->wTotalLength,
                        (addr_t)temp)
     < 0) {
    DLOG("Failed to get full descriptor");
    return FALSE;
  }

  DLOG("Got full descriptor");
  DLOG("cfg->bLength = %d", cfg->bLength);
  DLOG("sizeof(USB_CFG_DESC) = %d", sizeof(USB_CFG_DESC));
  
  interface = (USB_IF_DESC*)(((uint8_t*)temp) + cfg->bLength);
  
  while(interface->bDescriptorType != USB_TYPE_IF_DESC) {
    interface = (USB_IF_DESC*)(((uint8_t*)interface) + interface->bLength);
  }
  
  DLOG("interface length = %d, Descriptor Type = %d, ", interface->bLength,
       interface->bDescriptorType);

  gadget_dev->num_sub_devs = interface->bNumEndpoints;
  
  ept = (USB_EPT_DESC*)(((uint8_t*)interface) + interface->bLength);

  for(i = 0; i < gadget_dev->num_sub_devs; ++i) {

    gadget_sub_device_info_t* sub_dev = &gadget_dev->sub_devs[i];
    
    while(ept->bDescriptorType != USB_TYPE_EPT_DESC) {
      ept = (USB_EPT_DESC*)(((uint8_t*)ept) + ept->bLength);
    }
    
    DLOG("Endpoint address = %d, attributes = 0x%X, "
         "maxPacketSize = %d, interval = %d",
         ept->bEndpointAddress, ept->bmAttributes, ept->wMaxPacketSize,
         ept->bInterval);
    
    sub_dev->pipe = usb_create_pipe(device, ept);
    
    DLOG("sub_dev->pipe = 0x%X", sub_dev->pipe);

    DLOG("Pipe is %s", usb_is_endpoint_in(ept) ? "Input" : "Output");

    sub_dev->transaction_size = usb_payload_size(device, ept);
    DLOG("sub_dev->transaction_size = %d", sub_dev->transaction_size);
  

    if(usb_pipein(sub_dev->pipe)) {
      device->ep_in[usb_pipeendpoint(sub_dev->pipe)].desc = *ept;
    }
    else {
      device->ep_out[usb_pipeendpoint(sub_dev->pipe)].desc = *ept;
    }

    dev_num = usb_register_device(device, &gadget_driver, "beagle_communication");

    if(dev_num < 0) {
      DLOG("Failed to register device");
      panic("Failed to register device");
    }

    DLOG("Device number = %d", dev_num);
    
    if(i == 0) {
      gadget_dev->first_dev_num = dev_num;
    }

    
    ept = (USB_EPT_DESC*)(((uint8_t*)ept) + ept->bLength);
  }
  
  return TRUE;
}

bool usb_gadget_driver_init(void)
{
  return usb_register_driver(&gadget_driver);
}



#include "module/header.h"

static const struct module_ops mod_ops = {
  .init = usb_gadget_driver_init
};

DEF_MODULE (usb___beaglegadget, "USB gadget driver", &mod_ops, {"usb"});

/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
