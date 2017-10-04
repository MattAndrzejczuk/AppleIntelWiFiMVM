#include "AppleIntelWiFiMVM.h"
extern "C" {
#include "linux/linux-porting.h"
#include "linux/device-list.h"
}

#define AlwaysLog(args...) do {IOLog(MYNAME ":" args); } while(0)

#define super IOService
OSDefineMetaClassAndStructors(AppleIntelWiFiMVM, IOService);

// ------------------------ IOService Methods ----------------------------

bool AppleIntelWiFiMVM::init(OSDictionary *dict) {
    bool res = super::init(dict);
    DEBUGLOG(":init\n");
    return res;
}

bool AppleIntelWiFiMVM::start(IOService* provider) {
    AlwaysLog(":start\n");
    if(!super::start(provider)) {
        AlwaysLog(" Super start failed\n");
        return false;
    }

    // Ensure we have a PCI device provider
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if(!pciDevice) {
        AlwaysLog(" Provider not a PCIDevice\n");
        return false;
    }
    
    wl = getWorkLoop();
    

    UInt16 vendor = pciDevice->configRead16(kIOPCIConfigVendorID);
    UInt16 device = pciDevice->configRead16(kIOPCIConfigDeviceID);
    UInt16 subsystem_vendor = pciDevice->configRead16(kIOPCIConfigSubSystemVendorID);
    UInt16 subsystem_device = pciDevice->configRead16(kIOPCIConfigSubSystemID);
    UInt8 revision = pciDevice->configRead8(kIOPCIConfigRevisionID);
//    vendor = 0x8086;
//    subsystem_vendor = 0x8086;
    // Broadwell NUC 7265
//    device = 0x095a;
//    subsystem_device = 0x9510;
    // Skylake NUC 8260
//    device = 0x24F3;
//    subsystem_device = 0x9010;
    // 7260
//    device = 0x08B1;
//    subsystem_device = 0x4070;
    // 3160
//    device = 0x08B3;
//    subsystem_device = 0x0070;
    // 3165 uses 7165D firmware
//    device = 0x3165;
//    subsystem_device = 0x4010;
    // 4165 uses 8260 firmware above, not retested here

    if(vendor != 0x8086 || subsystem_vendor != 0x8086) {
        AlwaysLog(" Unrecognized vendor/sub-vendor ID %#06x/%#06x; expecting 0x8086 for both; cannot load driver.\n",
              vendor, subsystem_vendor);
        return false;
    }

//    DEBUGLOG("%s Vendor %#06x Device %#06x SubVendor %#06x SubDevice %#06x Revision %#04x\n", MYNAME, vendor, device, subsystem_vendor, subsystem_device, revision);
    const struct iwl_cfg *card = identifyWiFiCard(device, subsystem_device);
    if(!card) {
        AlwaysLog(" Card has the right device ID %#06x but unmatched sub-device ID %#06x; cannot load driver.\n",
              device, subsystem_device);
        return false;
    }
    AlwaysLog(" loading for device %s\n",card->name);
    
    // Create locks for synchronization
    firmwareLoadLock = IOLockAlloc();
    if (!firmwareLoadLock) {
        AlwaysLog(" Unable to allocate firmware load lock\n");
        return false;
    }
    
    pciDevice->retain();

    AlwaysLog(" Starting Firmware...\n");
    if(!startFirmware(card, NULL)) {// TODO: PCI transport
        AlwaysLog(" Unable to start firmware\n");
        return false;
    }
    
    pciDevice->setMemoryEnable(true);
    
    registerService();
    
    nameMatching(card->name);
    
    
    OSDictionary *firmwareInfo = new OSDictionary();
    OSString *firmwareVersion = new OSString();
    OSString *firmwareFile = new OSString();
    OSString *detectedDevice = new OSString();
    
    firmwareVersion->initWithCString(driver->fw.fw_version);
    firmwareFile->initWithCString(driver->firmware_name);
    detectedDevice->initWithCString(card->name);
    
    firmwareInfo->initWithCapacity(5);
    
    firmwareInfo->setObject("Detected Device",detectedDevice);
    firmwareInfo->setObject("Version", firmwareVersion);
    firmwareInfo->setObject("Firmware Name", firmwareFile);
    
    
    setProperty("Intel Firmware", firmwareInfo);
    

    return true;
}

void AppleIntelWiFiMVM::stop(IOService* provider) {
    DEBUGLOG(":stop\n");
    if(driver) stopFirmware();
    if (firmwareLoadLock)
    {
        IOLockFree(firmwareLoadLock);
        firmwareLoadLock = NULL;
    }
    super::stop(provider);
}

void AppleIntelWiFiMVM::free() {
    DEBUGLOG(":free\n");
    RELEASE(pciDevice);
    if(driver) IOFree(driver, sizeof(iwl_drv));
    super::free();
}

const struct iwl_cfg *AppleIntelWiFiMVM::identifyWiFiCard(UInt16 device, UInt16 subdevice) {
    UInt32 i;
    for(i=0; i<sizeof(wifi_card_ids) / sizeof(wifi_card); i++) {
        if(wifi_card_ids[i].device == device && wifi_card_ids[i].subdevice == subdevice)
            return wifi_card_ids[i].config;
    }
    
    return NULL;
}

OSObject* AppleIntelWiFiMVM::translateEntry(OSObject *obj){
    // Note: non-NULL result is retained...
    
    // if object is another array, translate it
    if (OSArray* array = OSDynamicCast(OSArray, obj))
        return translateArray(array);
    
    // if object is a string, may be translated to boolean
    if (OSString* string = OSDynamicCast(OSString, obj))
    {
        // object is string, translate special boolean values
        const char* sz = string->getCStringNoCopy();
        if (sz[0] == '>')
        {
            // boolean types true/false
            if (sz[1] == 'y' && !sz[2])
                return OSBoolean::withBoolean(true);
            else if (sz[1] == 'n' && !sz[2])
                return OSBoolean::withBoolean(false);
            // escape case ('»n' '»y'), replace with just string '>n' '>y'
            else if (sz[1] == '>' && (sz[2] == 'y' || sz[2] == 'n') && !sz[3])
                return OSString::withCString(&sz[1]);
        }
    }
    return NULL; // no translation
}

OSObject* AppleIntelWiFiMVM::translateArray(OSArray* array){
    // may return either OSArray* or OSDictionary*
    
    int count = array->getCount();
    if (!count)
        return NULL;
    
    OSObject* result = array;
    
    // if first entry is an empty array, process as array, else dictionary
    OSArray* test = OSDynamicCast(OSArray, array->getObject(0));
    if (test && test->getCount() == 0)
    {
        // using same array, but translating it...
        array->retain();
        
        // remove bogus first entry
        array->removeObject(0);
        -count;
        
        // translate entries in the array
        for (int i = 0; i < count; ++i)
        {
            if (OSObject* obj = translateEntry(array->getObject(i)))
            {
                array->replaceObject(i, obj);
                obj->release();
            }
        }
    }
    else
    {
        // array is key/value pairs, so must be even
        if (count & 1)
            return NULL;
        
        // dictionary constructed to accomodate all pairs
        int size = count >> 1;
        if (!size) size = 1;
        OSDictionary* dict = OSDictionary::withCapacity(size);
        if (!dict)
            return NULL;
        
        // go through each entry two at a time, building the dictionary
        for (int i = 0; i < count; i += 2)
        {
            OSString* key = OSDynamicCast(OSString, array->getObject(i));
            if (!key)
            {
                dict->release();
                return NULL;
            }
            // get value, use translated value if translated
            OSObject* obj = array->getObject(i+1);
            OSObject* trans = translateEntry(obj);
            if (trans)
                obj = trans;
            dict->setObject(key, obj);
            OSSafeRelease(trans);
        }
        result = dict;
    }
    
    // Note: result is retained when returned...
    return result;
}

