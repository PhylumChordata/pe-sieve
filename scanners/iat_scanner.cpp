#include "iat_scanner.h"

#include <peconv.h>

#include <fstream>

using namespace pesieve;

namespace pesieve {

	class ImportInfoCallback : public peconv::ImportThunksCallback
	{
	public:
		ImportInfoCallback(BYTE* _modulePtr, size_t _moduleSize, std::map<ULONGLONG, peconv::ExportedFunc> &_storedFunc)
			: ImportThunksCallback(_modulePtr, _moduleSize), storedFunc(_storedFunc)
		{
		}

		virtual bool processThunks(LPSTR lib_name, ULONG_PTR origFirstThunkPtr, ULONG_PTR firstThunkPtr)
		{
			if (this->is64b) {
				IMAGE_THUNK_DATA64* desc = reinterpret_cast<IMAGE_THUNK_DATA64*>(origFirstThunkPtr);
				ULONGLONG* call_via = reinterpret_cast<ULONGLONG*>(firstThunkPtr);
				return processThunks_tpl<ULONGLONG, IMAGE_THUNK_DATA64>(lib_name, desc, call_via, IMAGE_ORDINAL_FLAG64);
			}
			IMAGE_THUNK_DATA32* desc = reinterpret_cast<IMAGE_THUNK_DATA32*>(origFirstThunkPtr);
			DWORD* call_via = reinterpret_cast<DWORD*>(firstThunkPtr);
			return processThunks_tpl<DWORD, IMAGE_THUNK_DATA32>(lib_name, desc, call_via, IMAGE_ORDINAL_FLAG32);
		}

	protected:
		template <typename T_FIELD, typename T_IMAGE_THUNK_DATA>
		bool processThunks_tpl(LPSTR lib_name, T_IMAGE_THUNK_DATA* desc, T_FIELD* call_via, T_FIELD ordinal_flag)
		{
			ULONGLONG call_via_rva = ((ULONG_PTR)call_via - (ULONG_PTR)this->modulePtr);
			T_FIELD raw_ordinal = 0;
			bool is_by_ord = (desc->u1.Ordinal & ordinal_flag) != 0;
			if (is_by_ord) {
				raw_ordinal = desc->u1.Ordinal & (~ordinal_flag);
#ifdef _DEBUG
				std::cout << "raw ordinal: " << std::hex << raw_ordinal << std::endl;
#endif
				this->storedFunc[call_via_rva] = peconv::ExportedFunc(peconv::get_dll_shortname(lib_name), raw_ordinal);
			}
			else {
				PIMAGE_IMPORT_BY_NAME by_name = (PIMAGE_IMPORT_BY_NAME)((ULONGLONG)modulePtr + desc->u1.AddressOfData);
				LPSTR func_name = reinterpret_cast<LPSTR>(by_name->Name);
				raw_ordinal = by_name->Hint;
				this->storedFunc[call_via_rva] = peconv::ExportedFunc(peconv::get_dll_shortname(lib_name), func_name, raw_ordinal);
			}
			return true;
		}

		//fields:
		std::map<ULONGLONG, peconv::ExportedFunc> &storedFunc;
	};
	///----
}; //namespace pesieve

const bool IATScanReport::hooksToJSON(std::stringstream &outs, size_t level)
{
	if (notCovered.count() == 0) {
		return false;
	}
	bool is_first = true;
	OUT_PADDED(outs, level, "\"hooks_list\" : [\n");

	std::map<ULONGLONG, ULONGLONG>::iterator itr;
	for (itr = notCovered.thunkToAddr.begin(); itr != notCovered.thunkToAddr.end(); ++itr) {
		const ULONGLONG thunk = itr->first;
		const ULONGLONG addr = itr->second;
		if (!is_first) {
			outs << ",\n";
		}
		is_first = false;
		OUT_PADDED(outs, level, "{\n");

		OUT_PADDED(outs, (level + 1), "\"thunk_rva\" : ");
		outs << "\"" << std::hex << (ULONGLONG)thunk << "\"" << ",\n";

		std::map<ULONGLONG, peconv::ExportedFunc>::const_iterator found = storedFunc.find(thunk);
		if (found != storedFunc.end()) {
			const peconv::ExportedFunc &func = found->second;

			OUT_PADDED(outs, (level + 1), "\"func_name\" : ");
			outs << "\"" << func.toString() << "\"" << ",\n";
		}

		OUT_PADDED(outs, (level + 1), "\"target\" : ");
		outs << "\"" << std::hex << (ULONGLONG)addr << "\"";

		outs << "\n";
		OUT_PADDED(outs, level, "}");
	}
	outs << "\n";
	OUT_PADDED(outs, level, "]");
	return true;
}

bool IATScanReport::saveNotRecovered(IN std::string fileName,
	IN HANDLE hProcess,
	IN const std::map<ULONGLONG, peconv::ExportedFunc> *storedFunc,
	IN peconv::ImpsNotCovered &notCovered,
	IN const ModulesInfo &modulesInfo,
	IN const peconv::ExportsMapper *exportsMap)
{
	const char delim = ';';

	if (notCovered.count() == 0) {
		return false;
	}
	std::ofstream report;
	report.open(fileName);
	if (report.is_open() == false) {
		return false;
	}

	std::map<ULONGLONG,ULONGLONG>::iterator itr;
	for (itr = notCovered.thunkToAddr.begin(); itr != notCovered.thunkToAddr.end(); ++itr)
	{
		const ULONGLONG thunk = itr->first;
		const ULONGLONG addr = itr->second;
		report << std::hex << thunk << delim;

		if (storedFunc) {
			std::map<ULONGLONG, peconv::ExportedFunc>::const_iterator found = storedFunc->find(thunk);
			if (found != storedFunc->end()) {
				const peconv::ExportedFunc &func = found->second;
				report << func.toString();
			}
			else {
				report << "(unknown)";
			}
			report << "->";
		}

		if (exportsMap) {
			ScannedModule *modExp = modulesInfo.findModuleContaining(addr);
			ULONGLONG module_start = (modExp) ? modExp->getStart() : peconv::fetch_alloc_base(hProcess, (BYTE*)addr);

			const peconv::ExportedFunc* func = exportsMap->find_export_by_va(addr);
			if (func) {
				report << func->toString();
			}
			else {
				if (module_start == 0) {
					report << "(invalid)";
				}
				else {
					char moduleName[MAX_PATH] = { 0 };
					if (GetModuleBaseNameA(hProcess, (HMODULE)module_start, moduleName, sizeof(moduleName))) {
						report << peconv::get_dll_shortname(moduleName) << ".(unknown_func)";
					}
					else {
						report << "(unknown)";
					}
				}
			}

			size_t offset = addr - module_start;
			report << delim << std::hex << module_start << "+" << offset;

			if (modExp) {
				report << delim << modExp->isSuspicious();
			}
		}
		report << std::endl;
	}
	report.close();
	return true;
}

bool IATScanReport::generateList(IN const std::string &fileName, IN HANDLE hProcess, IN const ModulesInfo &modulesInfo, IN const peconv::ExportsMapper *exportsMap)
{
	return saveNotRecovered(fileName,
		hProcess,
		&storedFunc,
		notCovered,
		modulesInfo,
		exportsMap);
}

bool pesieve::IATScanner::hasImportTable(RemoteModuleData &remoteModData)
{
	if (!remoteModData.isInitialized()) {
		return false;
	}
	IMAGE_DATA_DIRECTORY *dir = peconv::get_directory_entry((BYTE*)remoteModData.headerBuffer, IMAGE_DIRECTORY_ENTRY_IMPORT);
	if (!dir) {
		return false;
	}
	if (dir->VirtualAddress > remoteModData.getHdrImageSize()) {
		std::cerr << "[-] Import Table out of scope" << std::endl;
		return false;
	}
	return true;
}

template <typename FIELD_T>
FIELD_T get_thunk_at_rva(BYTE *mod_buf, size_t mod_size, DWORD rva)
{
	if ( !mod_buf || !mod_size ) {
		return 0;
	}
	if ( !peconv::validate_ptr(mod_buf, mod_size, (BYTE*)((ULONG_PTR)mod_buf + rva), sizeof(FIELD_T)) ) {
		return 0;
	}

	FIELD_T* field_ptr = (FIELD_T*)((ULONG_PTR)mod_buf + rva);
	return (*field_ptr);
}

bool pesieve::IATScanner::scanByOriginalTable(peconv::ImpsNotCovered &not_covered)
{
	if (!remoteModData.isInitialized()) {
		std::cerr << "[-] Failed to initialize remote module header" << std::endl;
		return false;
	}

	if (!moduleData.isInitialized() && !moduleData.loadOriginal()) {
		std::cerr << "[-] Failed to initialize module data: " << moduleData.szModName << std::endl;
		return false;
	}

	// get addresses of the thunks from the original module (file)
	std::set<DWORD> origModThunks;
	if (!moduleData.loadImportThunks(origModThunks)) {
		return false;
	}

	// get filled thunks from the mapped module (remote):
	std::set<DWORD>::iterator itr;
	for (itr = origModThunks.begin(); itr != origModThunks.end(); ++itr) {
		DWORD thunk_rva = (*itr);
		//std::cout << "Thunk: " << std::hex << *itr << "\n";
		ULONGLONG next_thunk = 0;
		if (moduleData.is64bit()) {
			next_thunk = get_thunk_at_rva<ULONGLONG>(remoteModData.imgBuffer, remoteModData.imgBufferSize, thunk_rva);
		}
		else {
			next_thunk = get_thunk_at_rva<DWORD>(remoteModData.imgBuffer, remoteModData.imgBufferSize, thunk_rva);
		}
		// no export at this thunk:
		if (exportsMap.find_export_by_va(next_thunk) == nullptr) {
			not_covered.insert(thunk_rva, next_thunk);
		}
	}
	return true;
}

IATScanReport* pesieve::IATScanner::scanRemote()
{

	if (!remoteModData.isInitialized()) {
		std::cerr << "[-] Failed to initialize remote module header" << std::endl;
		return nullptr;
	}
	if (!hasImportTable(remoteModData)) {
		IATScanReport *report = new IATScanReport(processHandle, remoteModData.modBaseAddr, remoteModData.getModuleSize(), moduleData.szModName);
		report->status = SCAN_NOT_SUSPICIOUS;
		return report;
	}
	if (!remoteModData.loadFullImage()) {
		std::cerr << "[-] Failed to initialize remote module" << std::endl;
		return nullptr;
	}
	BYTE *vBuf = remoteModData.imgBuffer;
	size_t vBufSize = remoteModData.imgBufferSize;
	if (!vBuf) {
		return nullptr;
	}

	peconv::ImpsNotCovered not_covered;
	
	// first try to find by the Import Table in the original file:
	scanByOriginalTable(not_covered);

	// then try to find by coverage:
	peconv::fix_imports(vBuf, vBufSize, exportsMap, &not_covered);

	t_scan_status status = SCAN_NOT_SUSPICIOUS;
	if (not_covered.count() > 0) {
		status = SCAN_SUSPICIOUS;
	}

	IATScanReport *report = new IATScanReport(processHandle, remoteModData.modBaseAddr, remoteModData.getModuleSize(), moduleData.szModName);
	if (!report) {
		return nullptr;
	}

	if (not_covered.count()) {
		listAllImports(report->storedFunc);
	}
	if (this->hooksFilter != PE_IATS_UNFILTERED) {
		filterResults(not_covered, *report);
	}
	else {
		report->notCovered = not_covered;
	}
	report->status = status;
	if (report->countHooked() == 0) {
		report->status = SCAN_NOT_SUSPICIOUS;
	}
	return report;
}
///-------

void pesieve::IATScanner::initExcludedPaths()
{
	char sysWow64Path[MAX_PATH] = { 0 };
	ExpandEnvironmentStringsA("%SystemRoot%\\SysWoW64", sysWow64Path, MAX_PATH);
	this->m_sysWow64Path_str = sysWow64Path;
	std::transform(m_sysWow64Path_str.begin(), m_sysWow64Path_str.end(), m_sysWow64Path_str.begin(), tolower);

	char system32Path[MAX_PATH] = { 0 };
	ExpandEnvironmentStringsA("%SystemRoot%\\system32", system32Path, MAX_PATH);
	this->m_system32Path_str = system32Path;
	std::transform(m_system32Path_str.begin(), m_system32Path_str.end(), m_system32Path_str.begin(), tolower);
}

bool pesieve::IATScanner::filterResults(peconv::ImpsNotCovered &notCovered, IATScanReport &report)
{
	std::map<ULONGLONG, ULONGLONG>::iterator itr;
	for (itr = notCovered.thunkToAddr.begin(); itr != notCovered.thunkToAddr.end(); ++itr)
	{
		const ULONGLONG thunk = itr->first;
		const ULONGLONG addr = itr->second;

		ScannedModule *modExp = modulesInfo.findModuleContaining(addr);
		ULONGLONG module_start = (modExp) ? modExp->getStart() : peconv::fetch_alloc_base(this->processHandle, (BYTE*)addr);
		if (module_start == 0) {
			// invalid address of the hook
			report.notCovered.insert(thunk, addr);
			continue;
		}
		if (this->hooksFilter == PE_IATS_CLEAN_SYS_FILTERED) {
			// insert hooks leading to suspicious modules:
			if (modExp && modExp->isSuspicious()) {
				report.notCovered.insert(thunk, addr);
				continue;
			}
		}
		// filter out hooks leading to system DLLs
		char moduleName[MAX_PATH] = { 0 };
		if (GetModuleFileNameExA(this->processHandle, (HMODULE)module_start, moduleName, sizeof(moduleName))) {
			std::string dirName = peconv::get_directory_name(moduleName);
			std::transform(dirName.begin(), dirName.end(), dirName.begin(), tolower);
#ifdef _DEBUG
			std::cout << "Module dir name: " << dirName << "\n";
#endif
			if (dirName == m_system32Path_str || dirName == m_sysWow64Path_str) {
#ifdef _DEBUG
				std::cout << "Skipped: " << dirName << "\n";
#endif
				continue;
			}
		}
		// insert hooks leading to non-system modules:
		report.notCovered.insert(thunk, addr);
	}
	return true;
}

void pesieve::IATScanner::listAllImports(std::map<ULONGLONG, peconv::ExportedFunc> &_storedFunc)
{
	BYTE *vBuf = remoteModData.imgBuffer; 
	size_t vBufSize = remoteModData.imgBufferSize;
	if (!vBuf) {
		return;
	}
	ImportInfoCallback callback(vBuf, vBufSize, _storedFunc);
	peconv::process_import_table(vBuf, vBufSize, &callback);
}

