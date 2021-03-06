//by nightm4r3

/*
 * A sample, extra-simple block driver. Updated for kernel 2.6.31.
 *
 * (C) 2003 Eklektix, Inc.
 * (C) 2010 Pat Patterson <pat at superpat dot com>
 * Redistributable under the terms of the GNU GPL.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h> /* printk() */
#include <linux/fs.h>     /* everything... */
#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/crypto.h> //Linux Kernel Crypto API

MODULE_LICENSE("Dual BSD/GPL");
static char *Version = "1.4";

//Represent the key to be passed in during insmod
//insmod sbd key = "secret"
//This value should be hashed during initialization
static char *key = "default";

//charp: A char ointer value. Memory is allocated for user-provided strings,
//and the pointer is set accordingly
//S_IRUGO: Used for a parameter that can be read by the world but cannot be changed
module_param(key, charp, S_IRUGO);

static int major_num = 0;
module_param(major_num, int, 0);
static int logical_block_size = 512;
module_param(logical_block_size, int, 0);
static int nsectors = 1024; /* How big the drive is */
module_param(nsectors, int, 0);

//Crypto Variables:
struct crypto_cipher *tfm;
static char * stored_key = "CS444Projec3";
static int validKey = 1;

/*
 * We can tweak our hardware sector size, but the kernel talks to us
 * in terms of small sectors, always.
 */
#define KERNEL_SECTOR_SIZE 512

/*
 * Our request queue.
 */
static struct request_queue *Queue;

/*
 * The internal representation of our device.
 */
static struct sbd_device {
    unsigned long size;
    spinlock_t lock;
    u8 *data;
    struct gendisk *gd;
} Device;


//verbose purposes
void printk_hex(u8 * hex, int length) {
    int i;
    for (i = 0 ; i < length ; ++i) {
        printk("%02x", hex[i]);
    }
    printk("\n");
}

/*
 * Handle an I/O request.
 */
static void sbd_transfer(struct sbd_device *dev, sector_t sector,
    unsigned long nsect, char *buffer, int write) {
    unsigned long offset = sector * logical_block_size;
    unsigned long nbytes = nsect * logical_block_size;

    u8 * hex_print;
    int i = 0;

    if ((offset + nbytes) > dev->size) {
        printk (KERN_NOTICE "sbd: Beyond-end write (%ld %ld)\n", offset, nbytes);
        return;
    }

    if (write){
        printk("sbd.c: Writing: begin encrypting...\n");
        if(validKey){
        for (i = 0 ; i < nbytes ; i += crypto_cipher_blocksize(tfm))
            crypto_cipher_encrypt_one(tfm, dev->data + offset + i, buffer + i);
        }
        else
        {memcpy(dev->data + offset, buffer, nbytes);
        }
        if (i!=nbytes) printk("******************SBD block length is not fitted!!!****************"); 
        printk("sbd.c: original data:\n");
        hex_print = buffer;
        printk(hex_print, 15);

        printk("sbd.c: encrypted data:\n");
        hex_print = dev->data + offset;
        printk_hex(hex_print, 15);

//      memcpy(dev->data + offset, buffer, nbytes);
    }


    else { //Reading data
        printk("sbd.c: Reading: begin decrypting...\n");
        printk("sbd.c: given (encrypted) data:\n");
       hex_print = dev->data + offset;
        printk_hex(hex_print, 15);
   
        if(validKey){
            for (i = 0 ; i < nbytes ; i += crypto_cipher_blocksize(tfm)){
                crypto_cipher_decrypt_one(tfm, buffer + i, dev->data + offset + i);
            }
            if (i!=nbytes) printk("******************SBD block length is not fitted!!!****************"); 
        }
        else
        {  memcpy(buffer, dev->data + offset, nbytes);
        }   

        printk("sbd.c: decrypted data:\n");
        hex_print = buffer;
        printk_hex(hex_print, 15);

     // memcpy(buffer, dev->data + offset, nbytes);
    }

}

static void sbd_request(struct request_queue *q) {
    struct request *req;

    req = blk_fetch_request(q);
    while (req != NULL) {
        // blk_fs_request() was removed in 2.6.36 - many thanks to
        // Christian Paro for the heads up and fix...
        //if (!blk_fs_request(req)) {
        if (req == NULL || (req->cmd_type != REQ_TYPE_FS)) {
            printk (KERN_NOTICE "Skip non-CMD request\n");
            __blk_end_request_all(req, -EIO);
            continue;
        }
        sbd_transfer(&Device, blk_rq_pos(req), blk_rq_cur_sectors(req),
                bio_data(req->bio), rq_data_dir(req));
        if ( ! __blk_end_request_cur(req, 0) ) {
            req = blk_fetch_request(q);
        }
    }
}

/*
 * The HDIO_GETGEO ioctl is handled in blkdev_ioctl(), which
 * calls this. We need to implement getgeo, since we can't
 * use tools such as fdisk to partition the drive otherwise.
 */
int sbd_getgeo(struct block_device * block_device, struct hd_geometry * geo) {
    long size;

    /* We have no real geometry, of course, so make something up. */
    size = Device.size * (logical_block_size / KERNEL_SECTOR_SIZE);
    geo->cylinders = (size & ~0x3f) >> 6;
    geo->heads = 4;
    geo->sectors = 16;
    geo->start = 0;
    return 0;
}

/*
 * The device operations structure.
 */
static struct block_device_operations sbd_ops = {
        .owner  = THIS_MODULE,
        .getgeo = sbd_getgeo
};

static int __init sbd_init(void) {
    //Check key against stored_key

    
    if(key==NULL || strlen(key) == 0){
        key = stored_key;
        printk(KERN_WARNING "No vaild key - using stored_key");
    }
    

    /*
     * Set up our internal device.
     */
    Device.size = nsectors * logical_block_size;
    spin_lock_init(&Device.lock);
    Device.data = vmalloc(Device.size);
    if (Device.data == NULL)
        return -ENOMEM;
    /*
     * Get a request queue.
     */
    Queue = blk_init_queue(sbd_request, &Device.lock);
    if (Queue == NULL)
        goto out;
    blk_queue_logical_block_size(Queue, logical_block_size);
    /*
     * Get registered.
     */
    major_num = register_blkdev(major_num, "sbd");
    if (major_num < 0) {
        printk(KERN_WARNING "sbd: unable to get major number\n");
        goto out;
    }

    // Allocating Crypto API for AES cipher
    tfm = crypto_alloc_cipher("aes", 0, 0);
    if (tfm == 0) {
        printk(KERN_WARNING "sbd_enc: ERROR: unable to allocate tfm\n");
        goto out;
    }
    printk("sbd: KEY = %s\n", key);
    crypto_cipher_setkey(tfm, stored_key, strlen(stored_key));

    /*
     * And the gendisk structure.
     */
    Device.gd = alloc_disk(16);
    if (!Device.gd)
        goto out_unregister;
    Device.gd->major = major_num;
    Device.gd->first_minor = 0;
    Device.gd->fops = &sbd_ops;
    Device.gd->private_data = &Device;
    strcpy(Device.gd->disk_name, "sbd0");
    set_capacity(Device.gd, nsectors);
    Device.gd->queue = Queue;
    add_disk(Device.gd);

    return 0;

out_unregister:
    unregister_blkdev(major_num, "sbd");
out:
    vfree(Device.data);
    return -ENOMEM;
}

static void __exit sbd_exit(void)
{
    crypto_free_cipher(tfm); //freeing crypto key
    del_gendisk(Device.gd);
    put_disk(Device.gd);
    unregister_blkdev(major_num, "sbd");
    blk_cleanup_queue(Queue);
    vfree(Device.data);
}

module_init(sbd_init);
module_exit(sbd_exit);
