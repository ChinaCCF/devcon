#include <Windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <tchar.h>
#include <stdlib.h>

//0xe00xxx错误是 APPLICATION_ERROR_MASK|ERROR_SEVERITY_ERROR
//例如:
//#define ERROR_NO_DRIVER_SELECTED  (APPLICATION_ERROR_MASK|ERROR_SEVERITY_ERROR|0x203)

//arr_str 是 一连串字符串, 末端2个0
//这个函数返回字符串数组
wchar_t** convert_arr_str_2_arr(_In_ wchar_t* arr_str)
{
	int cnt = 0;
	auto p = arr_str;
	while (*p != 0)
	{
		cnt++;
		p += wcslen(p) + 1;
	}
	if (cnt == 0) return nullptr;

	wchar_t** arr = (wchar_t**)malloc((cnt + 1) * sizeof(void*));
	if (arr == nullptr) return nullptr;

	cnt = 0;
	arr[cnt] = arr_str;

	p = arr_str;
	while (*p != 0)
	{
		cnt++;
		p += wcslen(p) + 1;

		arr[cnt] = p;
	}

	arr[cnt] = nullptr;
	return arr;
}

//这个函数返回字符串数组
wchar_t** alloc_dev_property_arr(_In_ HDEVINFO devs, _In_ PSP_DEVINFO_DATA dev, _In_ DWORD hw_property)
{
	DWORD type;
	DWORD need_size;
	SetupDiGetDeviceRegistryPropertyW(devs, dev, hw_property, &type, nullptr, 0, &need_size);
	if (type != REG_MULTI_SZ) return nullptr;
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return nullptr;

	DWORD size = need_size;
	auto tmp = (wchar_t*)malloc(size + 2);
	if (!SetupDiGetDeviceRegistryPropertyW(devs, dev, hw_property, &type, (BYTE*)tmp, size, &need_size))
	{
		free(tmp);
		return nullptr;
	}

	auto arr = convert_arr_str_2_arr(tmp);
	if (arr) return arr;

	free(tmp);
	return nullptr;
}

void free_arr(_In_ wchar_t** arr)
{
	if (arr)
	{
		free(arr[0]);
		free(arr);
	}
}


bool cmp_arr_and_str(_In_ wchar_t** arr, _In_ const wchar_t* hw_id)
{
	if (arr)
	{
		while (arr[0])
		{
			if (_wcsicmp(arr[0], hw_id) == 0) 
				return true;

			arr++;
		}
	}
	return false;
}

typedef bool (*CallbackFunc)(_In_ HDEVINFO Devs, _In_ PSP_DEVINFO_DATA DevInfo, _In_ DWORD Index, _In_ LPVOID Context);
bool remove_driver(_In_ HDEVINFO devs, _In_ PSP_DEVINFO_DATA info, _In_ DWORD index, _In_ LPVOID ctx);

struct OperationContext
{
	wchar_t driver_name_[256]; 
	bool need_reboot_; 
};

bool enum_devs(CallbackFunc fun, _Inout_ LPVOID ctx)
{
	const wchar_t* hw_id = ((OperationContext*)ctx)->driver_name_;

	HDEVINFO devs = SetupDiGetClassDevsExW(nullptr,//driver class guid
										   NULL,
										   NULL,
										   DIGCF_ALLCLASSES | DIGCF_PRESENT,
										   NULL,
										   nullptr, //Machine,
										   NULL);


	if (devs == INVALID_HANDLE_VALUE) return false;

	int dev_index = 0;
	bool ret = false;
	do
	{
		SP_DEVINFO_DATA dev;
		dev.cbSize = sizeof(dev);
		if (!SetupDiEnumDeviceInfo(devs, dev_index, &dev)) break;
		dev_index++;

		wchar_t dev_id[MAX_DEVICE_ID_LEN];
		if (CM_Get_Device_IDW(dev.DevInst, dev_id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
			dev_id[0] = 0;

		wchar_t** hardware_id = alloc_dev_property_arr(devs, &dev, SPDRP_HARDWAREID);
		wchar_t** compat_id = alloc_dev_property_arr(devs, &dev, SPDRP_COMPATIBLEIDS);

		if (cmp_arr_and_str(hardware_id, hw_id) || cmp_arr_and_str(compat_id, hw_id))
		{ 
			ret = fun(devs, &dev, dev_index, ctx);
		}

		free_arr(hardware_id);
		free_arr(compat_id);

	} while (true);

	if (devs != INVALID_HANDLE_VALUE) 
		SetupDiDestroyDeviceInfoList(devs);

	return ret;
}


bool _do_setup_para(_In_ HDEVINFO devs, _In_ PSP_DEVINFO_DATA dev, _In_ SP_CLASSINSTALL_HEADER* header, _In_ DWORD size, DI_FUNCTION cmd)
{
	if (!SetupDiSetClassInstallParamsW(devs, dev, header, size))
	{
		wprintf(L"SetupDiSetClassInstallParamsW fail : 0x%x\n", GetLastError());
		return false;
	}

	if (!SetupDiCallClassInstaller(cmd, devs, dev))
	{
		wprintf(L"SetupDiCallClassInstaller fail : 0x%x\n", GetLastError());
		return false;
	}
	return true;
}

bool set_device_para(_In_ HDEVINFO devs, _In_ PSP_DEVINFO_DATA dev, _In_ DWORD scope, _In_ DWORD code)
{
	SP_PROPCHANGE_PARAMS para;

	para.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
	para.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
	para.StateChange = code;
	para.Scope = scope;
	para.HwProfile = 0;

	return _do_setup_para(devs, dev, &para.ClassInstallHeader, sizeof(para), DIF_PROPERTYCHANGE);
}

bool get_device_para(_In_ HDEVINFO devs, _In_ PSP_DEVINFO_DATA dev, _Inout_ LPVOID ctx)
{
	SP_DEVINSTALL_PARAMS para;
	para.cbSize = sizeof(para);
	if (!SetupDiGetDeviceInstallParamsW(devs, dev, &para))
	{
		wprintf(L"SetupDiGetDeviceInstallParamsW fail : 0x%x\n", GetLastError());
		return false;
	}

	auto pctx = (OperationContext*)ctx;
	if (para.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT))
		pctx->need_reboot_ = TRUE;
	return true;
}

bool remove_driver(_In_ HDEVINFO devs, _In_ PSP_DEVINFO_DATA dev, _In_ DWORD index, _In_ LPVOID ctx)
{
	(index);
	wprintf(L"to stop driver\n");
	if (!set_device_para(devs, dev, DICS_FLAG_CONFIGSPECIFIC, DICS_STOP)) return false;

	wprintf(L"to disable driver\n");
	if (!set_device_para(devs, dev, DICS_FLAG_GLOBAL, DICS_DISABLE)) return false;

	SP_REMOVEDEVICE_PARAMS para;
	para.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
	para.ClassInstallHeader.InstallFunction = DIF_REMOVE;
	para.Scope = DI_REMOVEDEVICE_GLOBAL;
	para.HwProfile = 0;
	if (!_do_setup_para(devs, dev, &para.ClassInstallHeader, sizeof(para), DIF_REMOVE)) return false;

	return get_device_para(devs, dev, ctx);
}


int wmain(int argc, const wchar_t* argv[])
{
	(argc);
	(argv);
	if (argc <= 1)
	{
		wprintf(L"args no enough! \n sample : hy_devcon \"root\\hidriver\"\n");
		return 0;
	}

	wprintf(L"to uninstall "); wprintf(argv[1]); wprintf(L"\n");

	OperationContext ctx;
	ctx.need_reboot_ = false;
	wcscpy_s(ctx.driver_name_, 256, argv[1]);
	if (!enum_devs(remove_driver, &ctx)) 
		wprintf(L"no driver operation\n"); 
	else
		wprintf(L"uninstall success\n");
	return 0;
}