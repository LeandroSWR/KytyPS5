#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

LIB_VERSION("NpCppWebApi", 1, "NpCppWebApi", 1, 1);

namespace NpCppWebApi {

// sce::Np::CppWebApi::Common::InitParams::InitParams()
static void KYTY_SYSV_ABI InitParamsCtor(void* self) {
	PRINT_NAME();
	LOGF("\t this = %p\n", self);
}

// sce::Np::CppWebApi::Common::InitParams::~InitParams()
static void KYTY_SYSV_ABI InitParamsDtor(void* self) {
	PRINT_NAME();
	LOGF("\t this = %p\n", self);
}

// sce::Np::CppWebApi::Common::LibContext::LibContext()
static void KYTY_SYSV_ABI LibContextCtor(void* self) {
	PRINT_NAME();
	LOGF("\t this = %p\n", self);
}

// sce::Np::CppWebApi::Common::initialize(const InitParams&, LibContext&)
static int KYTY_SYSV_ABI CommonInitialize(const void* params, void* context) {
	PRINT_NAME();
	LOGF("\t params  = %p\n"
	     "\t context = %p\n",
	     params, context);
	// Offline titles only need a successful startup. There is no PSN session to open.
	return OK;
}

} // namespace NpCppWebApi

LIB_DEFINE(InitNpCppWebApi_1) {
	LIB_FUNC("8x++mBOUeso", NpCppWebApi::InitParamsCtor);
	LIB_FUNC("52AlYvq+dmk", NpCppWebApi::InitParamsDtor);
	LIB_FUNC("Y295ygEccqk", NpCppWebApi::LibContextCtor);
	LIB_FUNC("UYPxv8MIzGo", NpCppWebApi::CommonInitialize);
}

} // namespace Libs
