detect the usb drive by listing /dev/disk/by-id/usb-*

with specific keyword we filter the devices

we get the device size and let the user to select the device, filter out those less than 128mb.

first unmount the device, then mount the device to a given path, configurable.

then we might want to write a file called device_info.cfg, with specific name, uuid and "last sync time" written in it

uuid shall be generated. name shall be input by user, non empty. last sync time shall be set to last access time? or the time the cfg file updated or created. 
