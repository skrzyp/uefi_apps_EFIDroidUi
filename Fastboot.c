#include <Library/CacheMaintenanceLib.h>

#include "EFIDroidUi.h"

#define STATE_OFFLINE	0
#define STATE_COMMAND	1
#define STATE_COMPLETE	2
#define STATE_ERROR	3

typedef struct _FASTBOOT_COMMAND FASTBOOT_COMMAND;
struct _FASTBOOT_COMMAND {
	struct _FASTBOOT_COMMAND *Next;
	CONST CHAR8 *Prefix;
	UINT32 PrefixLen;
	VOID (*Handle)(CHAR8 *Arg, VOID *Data, UINT32 Size);
};

typedef struct _FASTBOOT_VAR FASTBOOT_VAR;
struct _FASTBOOT_VAR {
	struct _FASTBOOT_VAR *Next;
	CONST CHAR8 *Name;
	CONST CHAR8 *Value;
};

STATIC EFI_EVENT mExitBootServicesEvent;
STATIC EFI_EVENT mUsbOnlineEvent;
STATIC lkapi_usbgadget_iface_t* mUsbInterface = NULL;
STATIC UINT32 mFastbootState = STATE_OFFLINE;
STATIC VOID *DownloadBase = NULL;
STATIC UINT32 DownloadSize = 0;
STATIC FASTBOOT_COMMAND *CommandList;
STATIC FASTBOOT_VAR *VariableList;

STATIC VOID
FastbootNotify (
    lkapi_udc_gadget_t *Gadget,
    UINT32 Event
)
{
  if (Event == LKAPI_UDC_EVENT_ONLINE) {
    gBS->SignalEvent (mUsbOnlineEvent);
  }
}

STATIC lkapi_udc_device_t surf_udc_device = {
	.vendor_id    = 0x18d1,
	.product_id   = 0xD00D,
	.version_id   = 0x0100,
	.manufacturer = "Google",
	.product      = "Android",
};

STATIC lkapi_udc_gadget_t fastboot_gadget = {
	.notify        = FastbootNotify,
	.ifc_class     = 0xff,
	.ifc_subclass  = 0x42,
	.ifc_protocol  = 0x03,
	.ifc_endpoints = 2,
	.ifc_string    = "fastboot",
};

STATIC VOID
FastbootAck (
  CONST CHAR8 *Code,
  CONST CHAR8 *Reason,
  BOOLEAN ChangeState
)
{
	CHAR8* Response = AllocatePool(FASTBOOT_COMMAND_MAX_LENGTH);
  ASSERT(Response);

	if (mFastbootState != STATE_COMMAND)
		return;

	if (Reason == 0)
		Reason = "";

	AsciiSPrint(Response, FASTBOOT_COMMAND_MAX_LENGTH, "%a%a", Code, Reason);

	if(fastboot_gadget.usb_write(&fastboot_gadget, Response, AsciiStrLen(Response))<0)
    mFastbootState = STATE_ERROR;
  else if(ChangeState)
  	mFastbootState = STATE_COMPLETE;

  FreePool(Response);
}

VOID
FastbootInfo (
  CONST CHAR8 *Reason
)
{
  FastbootAck("INFO", Reason, FALSE);
}

VOID
FastbootFail (
  CONST CHAR8 *Reason
)
{
	FastbootAck("FAIL", Reason, TRUE);
}

VOID
FastbootOkay (
  CONST CHAR8 *Info
)
{
	FastbootAck("OKAY", Info, TRUE);
}

VOID
FastbootRegister (
  CHAR8 *Prefix,
  VOID (*Handle)(CHAR8 *Arg, VOID *Data, UINT32 Size)
)
{
	FASTBOOT_COMMAND *Command;

	Command = AllocatePool(sizeof(*Command));
	if (Command) {
		Command->Prefix = Prefix;
		Command->PrefixLen = AsciiStrLen(Prefix);
		Command->Handle = Handle;
		Command->Next = CommandList;
		CommandList = Command;
	}
}

VOID
FastbootPublish (
  CONST CHAR8 *Name,
  CONST CHAR8 *Value
)
{
	FASTBOOT_VAR *Variable;

	Variable = AllocatePool(sizeof(*Variable));
	if (Variable) {
		Variable->Name = Name;
		Variable->Value = Value;
		Variable->Next = VariableList;
		VariableList = Variable;
	}
}

STATIC VOID
GetVarAll (
  VOID
)
{
	FASTBOOT_VAR *Variable;
	char Buffer[64];

	for (Variable = VariableList; Variable; Variable = Variable->Next)
	{
		AsciiStrnCpyS(Buffer, sizeof(Buffer), Variable->Name, AsciiStrLen(Variable->Name));
		AsciiStrCatS(Buffer, sizeof(Buffer), ":");
		AsciiStrCatS(Buffer, sizeof(Buffer), Variable->Value);

		FastbootInfo(Buffer);
		SetMem(Buffer, sizeof(Buffer), '\0');
	}
	FastbootOkay("");
}

STATIC VOID
CommandGetVar (
  CHAR8 *Arg,
  VOID *Data,
  UINT32 Size
)
{
	FASTBOOT_VAR *Variable;
	BOOLEAN All = FALSE;
	CHAR8 Response[128];

	All = !AsciiStrCmp("all", Arg);

	if (!AsciiStrnCmp("all", Arg, AsciiStrLen(Arg)))
	{
		GetVarAll();
		return;
	}

	for (Variable = VariableList; Variable; Variable = Variable->Next) {
		if (All) {
			AsciiSPrint(Response, sizeof(Response), "\t%a: [%a]", Variable->Name, Variable->Value);
			FastbootInfo(Response);
		}
		else if (!AsciiStrCmp(Variable->Name, Arg)) {
			FastbootOkay(Variable->Value);
			return;
		}
	}
	FastbootOkay("");
}

STATIC VOID
CommandHelp (
  CHAR8 *Arg,
  VOID *Data,
  UINT32 Size
)
{
	FASTBOOT_COMMAND *Command;
	CHAR8 Response[128];

	// print commands
	FastbootInfo("commands:");
	for (Command = CommandList; Command; Command = Command->Next) {
		AsciiSPrint(Response, sizeof(Response), "\t%a", Command->Prefix);
		FastbootInfo(Response);
	}

	FastbootOkay("");
}

/* todo: give lk strtoul and nuke this */
STATIC UINT32
HexToUnsigned (
  CONST CHAR8 *x
)
{
    UINT32 n = 0;

    while(*x) {
        switch(*x) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            n = (n << 4) | (*x - '0');
            break;
        case 'a': case 'b': case 'c':
        case 'd': case 'e': case 'f':
            n = (n << 4) | (*x - 'a' + 10);
            break;
        case 'A': case 'B': case 'C':
        case 'D': case 'E': case 'F':
            n = (n << 4) | (*x - 'A' + 10);
            break;
        default:
            return n;
        }
        x++;
    }

    return n;
}

STATIC VOID
CommandDownload (
  CHAR8 *Arg,
  VOID *Data,
  UINT32 Size
)
{
  CHAR8 Response[FASTBOOT_COMMAND_MAX_LENGTH];
  UINT32 Length = HexToUnsigned(Arg);
  INT32 r;

  // free old data
  if(DownloadBase) {
    FreePool(DownloadBase);
  }

  // allocate data buffer
	DownloadSize = 0;
  DownloadBase = AllocatePool(Length);
  if(DownloadBase == NULL) {
		FastbootFail("data too large");
		return;
  }

  // write response
	AsciiSPrint(Response, FASTBOOT_COMMAND_MAX_LENGTH, "DATA%08x", Length);
	if(fastboot_gadget.usb_write(&fastboot_gadget, Response, AsciiStrLen(Response))<0) {
    mFastbootState = STATE_ERROR;
		return;
  }

	// Discard the cache contents before starting the download
	InvalidateDataCacheRange(DownloadBase, Length);

  // read data
	r = fastboot_gadget.usb_read(&fastboot_gadget, DownloadBase, Length);
	if ((r < 0) || ((UINT32) r != Length)) {
		mFastbootState = STATE_ERROR;
		return;
	}

  // set size and send OKAY
	DownloadSize = Length;
	FastbootOkay("");
}

STATIC
VOID
FastbootCommandLoop (
  VOID
)
{
  INT32 r;
	FASTBOOT_COMMAND *Command;
  printf("fastboot: processing commands\n");

  UINT8* Buffer = AllocatePool(4096);
  ASSERT(Buffer);

AGAIN:
  while (mFastbootState != STATE_ERROR) {
    SetMem(Buffer, FASTBOOT_COMMAND_MAX_LENGTH, 0);
		InvalidateDataCacheRange(Buffer, FASTBOOT_COMMAND_MAX_LENGTH);

    r = fastboot_gadget.usb_read(&fastboot_gadget, Buffer, FASTBOOT_COMMAND_MAX_LENGTH);
		if (r < 0) break;

    // Commands aren't null-terminated. Let's get a null-terminated version.
		Buffer[r] = 0;

    mFastbootState = STATE_COMMAND;

		for (Command = CommandList; Command; Command = Command->Next) {
			if (CompareMem(Buffer, Command->Prefix, Command->PrefixLen))
				continue;

			CHAR8* Arg = (CHAR8*) Buffer + Command->PrefixLen;
			if(Arg[0]==' ')
				Arg++;

			Command->Handle(Arg, DownloadBase, DownloadSize);
			if (mFastbootState == STATE_COMMAND)
				FastbootFail("unknown reason");
			goto AGAIN;
		}

		FastbootInfo("unknown command");
		FastbootInfo("See 'fastboot oem help'");
		FastbootFail("");
  }

	mFastbootState = STATE_OFFLINE;
	printf("fastboot: oops!\n");
	FreePool(Buffer);

  if (DownloadBase!=NULL) {
    FreePool(DownloadBase);
    DownloadBase = NULL;
    DownloadSize = 0;
  }
}

STATIC
VOID
FastbootHandler (
  VOID
)
{
	for (;;) {
    UINTN EventIndex;
    gBS->WaitForEvent (1, &mUsbOnlineEvent, &EventIndex);

		FastbootCommandLoop();
	}
}

STATIC
VOID
EFIAPI
ExitBootServicesEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mUsbInterface->udc_stop(mUsbInterface);
}

VOID
FastbootInit (
  VOID
)
{
  EFI_STATUS Status;
  lkapi_t *LKApi = GetLKApi();

  Status = gBS->CreateEvent (0, TPL_CALLBACK, NULL, NULL, &mUsbOnlineEvent);
  ASSERT_EFI_ERROR (Status);

  mUsbInterface = LKApi->usbgadget_get_interface();
  ASSERT(mUsbInterface);

	FastbootRegister("oem help", CommandHelp);
	FastbootRegister("getvar:", CommandGetVar);
	FastbootRegister("download:", CommandDownload);
  FastbootPublish("version", "0.5");

  surf_udc_device.serialno = AsciiStrDup("EFIDroid");
  ASSERT(mUsbInterface->udc_init(mUsbInterface, &surf_udc_device)==0);
	ASSERT(mUsbInterface->udc_register_gadget(mUsbInterface, &fastboot_gadget)==0);
  ASSERT(mUsbInterface->udc_start(mUsbInterface)==0);

  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY, ExitBootServicesEvent, NULL, &mExitBootServicesEvent);
  ASSERT_EFI_ERROR (Status);

  FastbootHandler();

  Status = gBS->CloseEvent(mExitBootServicesEvent);
  mUsbInterface->udc_stop(mUsbInterface);
}
