#ifndef EMS_H
#define EMS_H
#define SEN_TRAY sensor->id <16? trayName[tray_1_ID].c_str() : trayName[tray_2_ID].c_str()
#define SEN_ID (sensor->id % 16) + 1
#define SQL_RESULT_LEN 240
#define SQL_RETURN_CODE_LEN 1000
#include "implot.h"
#include "implot_internal.h"
#include <Windows.h>
#include <string>
#include <fstream>
#include <thread>
#include <algorithm>
#include <iostream>
#include <vector>
#include <stdio.h>
#include "loguru.hpp"
#include <tchar.h>
#include <strsafe.h>
#include <dbt.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
//#include <boost/locale.hpp>
#include <sqlext.h>
#include <sqltypes.h>
#include <sql.h>
#include <sstream>
#include <iomanip>
#include <mutex>
extern int tick;
struct ScrollingBuffer {
    int MaxSize;
    int Offset;
    ImVector<ImVec2> Data;
    ScrollingBuffer(int max_size = 1000) {
        MaxSize = max_size;
        Offset = 0;
        Data.reserve(MaxSize);
    }
    void AddPoint(float x, float y) {
        if (Data.size() < MaxSize)
            Data.push_back(ImVec2(x, y));
        else {
            Data[Offset] = ImVec2(x, y);
            Offset = (Offset + 1) % MaxSize;
        }
    }
    void Erase() {
        if (Data.size() > 0) {
            Data.shrink(0);
            Offset = 0;
        }
    }
};

struct sensorStruct {
    int invalidReadCount = 0;
    bool faultyCOM = true;
    std::string Assembled_Ident_Number = "";
    std::string  Type_ID = "0";
    int Decommissioned;
    std::string Works_Order_Number = "0";
    std::string Revision = "0";
    int Current_Software_ID;

    std::string Device_Type;
    std::string Brand_ID;
    std::string Equivalent_Type_ID;
    std::string EFACS_Part_Number;

    std::string Mainboard_ID;
    int Tests_Passed;

    bool validRead = false;
    int disconnectedCounter = 0;
    std::string COM = "";
    char** MBreadArray = (char**)malloc(14 * sizeof(char*));
    std::string mrReceive = "";
    char receive[512] = "";
    bool activated = false;
    bool fCTS = false;
    int TT1 = 0;
    int TT2 = 0;
    int TT3 = 0;
    int TT4 = 0;
    int TT5 = 0;
    int SMOKE = 0;
    int SMOKE_STATUS = 0;
    int BASE = 0;
    int ORIGINAL_BASE = 0;
    int LIGHT_REF = 0;
    int LIGHT_VALUE = 0;
    int DARK_REF = 0;
    int DARK_VALUE = 0;
    int id;
    HANDLE hSerial = NULL;
    std::string serial = "";
    std::vector<std::string> queue;
    bool isClosed = true;
    int deviceType = 4; // 0 = utc/security, 1 = dual, 2 = sounder, 3 = beacon, 4 = unknown
    std::string testPassed = "N/A"; // FAIL, SUCCESS, SQL FAIL, SQL NOT INSERTED, REINSERT, PREVIOUS TEST NOT PASSED
    ScrollingBuffer sdata; //smoke plot data
    ScrollingBuffer sdata2; //smoke/baseline plot data
    int completedTest = 0;
    int completedSegment = 0;
    bool openingPorts = false;
    bool sendRTS = true;
};

struct calibrationPresets {
    std::string testName = "Test 1";
    std::string tunnelCommand = "s01";
    std::string tunnelCommand2 = "s02";
    int readFromScatter = false;
    int testType = 0;
    float min = 0.198f;
    float max = 0.202f;
    int samples = 30;
    float preAlarmSetPoint = 0.077f;
    float alarmSetPoint = 0.086f;
    float UTC_preAlarmSetPoint = 0.098f;
    float UTC_alarmSetPoint = 0.108f;
    float min_smartcell = 0.103f; //for alarm test
    float max_smartcell = 0.128f;
    float min_utc = 0.124f;
    float max_utc = 0.15f;
    int id = 1;
};

struct userStruct {
    std::string firstName = "null";
    std::string lastName = "null";
    std::string serial = "0000";
};


extern sensorStruct sen[32];


extern float REFRESH_RATE;
bool isNumber(const std::string& str);
bool isFloatNumber(const std::string& string);
void tokenize(std::string& str, char delim, std::vector<std::string>& out);
int sendBytes(sensorStruct* sen);
int sendReadBytesTunnel();
int COMSetup(bool firstTimeSetup);
int setAlarmThreshold(sensorStruct* sen, int* inUse, calibrationPresets preset, int noSensors, bool skipSql);
int alarmTest(sensorStruct* sensor, int* status, calibrationPresets preset, int noSensors, bool skipSql);
BOOL EnumerateComPortQueryDosDevice(int* pNumber, char** pPortName);
inline std::string getCurrentDateTime(std::string s);
void init();
int saveSmokeToCSV();
int updateSmokeCSVHeader();
int startCalibration(int test, int* calibrating, bool skipSql, std::string* previousTest, std::string* currentTest, bool* tunnelPurged);
int calibrateSensors(int test, int* calibrating, bool skipSql);
int stopCalibration(int* calibrating, std::string* previousTest, bool* tunnelEmptying);
void TextCenter(const char* format, ...);
void StyleSeaborn();
static void HelpMarker(const char* desc);
int loadTestPresets(calibrationPresets* presets);
int createCalibrationPresets();
int saveCalibrationPresets(calibrationPresets* presets);
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
BOOL DoRegisterDeviceInterfaceToHwnd(IN GUID InterfaceClassGuid, IN HWND hWnd, OUT HDEVNOTIFY* hDeviceNotify);
int closeHandleWrapper(sensorStruct* sen);
int generateTraySetup();
int COMRescan();
int changeLanguage(int lang);
int openPorts(sensorStruct* sensor);
int disconnectFromSQLServer(SQLHANDLE* sqlStmtHandle, SQLHDBC* sqlConnHandle, SQLHANDLE* sqlEnvHandle);
std::vector<std::string> ExecuteSql(const CHAR* sql, int* status);
void DisplayError(SQLSMALLINT t, SQLHSTMT h);
void GetMachineName(char machineName[150]);
template< typename T > std::string int_to_hex(T i);
void TextCenterColored(const ImVec4& col, const char* fmt, ...);
#endif