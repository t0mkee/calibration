/*
make sure to warn user if too many port opens
//check notify
*/
#include "ems.h"
#include "LogHelper.h"
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
#include <GL/gl3w.h>            
#endif
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WGL
#include <GLFW/glfw3native.h>
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define gettext(x) x
//#define gettext(x) gettext(x).c_str()
#define SENSORS 32
#define BATCH_SIZE 30
#define VERSION "1.1.3"
#define SOFTWARE_NAME "EMS Sensor Calibration"
#define ASK_LOGIN TRUE // MAY CAUSE ISSUES LIKE TUNNEL PURGING IF FALSE

//using namespace boost::locale;
bool inComSetup = false;
bool tunnelPause = false;
bool run = false;
float REFRESH_RATE = 1.0f; // how many updates a second, for debugging purposes only
std::string tunnelReadings[15];
std::string tunnelCOM = "";
static const char* COM_selectable[32]; // UI click and drag elements in com port setup
sensorStruct sen[32];
bool serialHeaderChanged = false; //notify if the smoke.csv header needs to be updated when new sensors are added
std::vector<std::string> tunnelCommands; // tunnel command queue issued by user
bool tunnelConnected = false;
GUILog guiLog; // gui component for log in engineer's debug mode
std::string trayName[5] = { "UTC A", "UTC B", "SMARTCELL A", "SMARTCELL B", "UNKNOWN TRAY" };
int tray_1_ID = 4; // tray id used for gui component in tray setup
int tray_2_ID = 4;
int tick = 0; //used only for plotting in graph window 
int uptimeSeconds = 0;
int uptimeFrames = 0;
userStruct user; // track user 
std::chrono::steady_clock::time_point Idle_Time_Begin; //track idle time between tests for sql entries
std::chrono::steady_clock::time_point Idle_Time_End;
bool LoadTextureFromFile(const char* filename, GLuint* out_texture, int* out_width, int* out_height);
//***********************************************************************//
static void glfw_error_callback(int error, const char* description)
{
	LOG_F(2, "Glfw Error %d: %s\n", error, description);
}
// Simple helper function to load an image into a OpenGL texture with common settings
bool LoadTextureFromFile(const char* filename, GLuint* out_texture, int* out_width, int* out_height)
{
	// Load from file
	int image_width = 0;
	int image_height = 0;
	unsigned char* image_data = stbi_load(filename, &image_width, &image_height, NULL, 4);
	if (image_data == NULL)
		return false;

	// Create a OpenGL texture identifier
	GLuint image_texture;
	glGenTextures(1, &image_texture);
	glBindTexture(GL_TEXTURE_2D, image_texture);

	// Setup filtering parameters for display
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // This is required on WebGL for non power-of-two textures
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // Same

	// Upload pixels into texture
#if defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
	stbi_image_free(image_data);

	*out_texture = image_texture;
	*out_width = image_width;
	*out_height = image_height;

	return true;
}
void init() {
	Idle_Time_Begin = std::chrono::steady_clock::now();
	char* dummy[1] = { "" };
	int dummyint = 0;
	loguru::init(dummyint, dummy);
	std::string filePath = "log/log_" + getCurrentDateTime("date") + ".txt";
	loguru::add_file(filePath.c_str(), loguru::Append, loguru::Verbosity_MAX);
	std::ifstream file("config/test_presets.txt");
	if (!file.is_open()) {
		LOG_F(1, gettext("Test presets not found, generating new file"));
		createCalibrationPresets();
	}
	else
		file.close();
	CreateDirectoryA("log", NULL);
	CreateDirectoryA("config", NULL);
	CreateDirectoryA("smoke", NULL);
	for (int i = 0; i < SENSORS; i++)
		sen[i].id = i;
	COMSetup(true);
	LOG_F(0, "Using %s as tunnel COM", tunnelCOM.c_str());
	for (int i = 0; i < 14; i++)
		for (int j = 0; j < SENSORS; j++)
			sen[j].MBreadArray[i] = (char*)malloc(5); // malloc eachs sensor read array for mb command //check replace with c++ new
	for (int i = 0; i < SENSORS; i++)
		COM_selectable[i] = sen[i].COM.c_str();
	for (int a = 0; a < 15; a++)
		tunnelReadings[a] = "N/A";

}
struct file_loader {
	std::vector<char> operator()(std::string const& name, std::string const&/*encoding*/) const
	{
		std::vector<char> buffer;
		std::ifstream f(name.c_str(), std::ifstream::binary);
		if (!f)
			return buffer;
		f.seekg(0, std::ifstream::end);
		size_t len = f.tellg();
		if (len == 0)
			return buffer;
		f.seekg(0);
		buffer.resize(len, '\0');
		f.read(&buffer[0], len);
		return buffer;
	}
};
int disconnectFromSQLServer(SQLHANDLE* sqlStmtHandle, SQLHDBC* sqlConnHandle, SQLHANDLE* sqlEnvHandle) {
	SQLFreeHandle(SQL_HANDLE_STMT, sqlStmtHandle);
	SQLDisconnect(sqlConnHandle);
	SQLFreeHandle(SQL_HANDLE_DBC, sqlConnHandle);
	SQLFreeHandle(SQL_HANDLE_ENV, sqlEnvHandle);
	return 0;
}

int __stdcall _tWinMain(_In_ HINSTANCE hInstanceExe, _In_opt_ HINSTANCE, _In_ PTSTR lpstrCmdLine, _In_ int nCmdShow) {


	static const char* class_name = "DUMMY_CLASS";
	WNDCLASSEX wx = {};
	wx.cbSize = sizeof(WNDCLASSEX);
	wx.lpfnWndProc = WindowProc;        // function which will handle messages
	wx.hInstance = hInstanceExe;
	wx.lpszClassName = class_name;

	if (!RegisterClassEx(&wx))
	{
		return 0;
	}
	HWND msgOnlyWnd = CreateWindowEx( //message only window to capture status of com ports
		NULL,
		class_name,
		"message only window",
		0, 0, 0,
		0, 0,
		0, NULL,
		NULL,
		NULL);

	if (msgOnlyWnd == NULL)
	{
		return -1;
	}
	int lang = 0;

	init();

	// Setup window
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
		return 1;

	// GL 3.0 + GLSL 130
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	const GLFWvidmode* mode = glfwGetVideoMode(monitor);

	glfwWindowHint(GLFW_RED_BITS, mode->redBits);
	glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
	glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
	//glfwWindowHint(GLFW_REFRESH_RATE, 60);
	GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, SOFTWARE_NAME, monitor, NULL);
	glfwSetWindowMonitor(window, NULL, 0, 25, mode->width, mode->height, 144);
	if (window == NULL)
		return 1;
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1); // Enable vsync

	// Initialize OpenGL loader
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
	bool err = gl3wInit() != 0;
#else
	bool err = false; // If you use IMGUI_IMPL_OPENGL_LOADER_CUSTOM, your loader is likely to requires some form of initialization.
#endif
	if (err)
	{
		LOG_F(2, "Failed to initialize OpenGL loader!\n");
		return 1;
	}
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	ImFont* font = io.Fonts->AddFontDefault();

	// Add character ranges and merge into the previous font
	// The ranges array is not copied by the AddFont* functions and is used lazily
	// so ensure it is available at the time of building or calling GetTexDataAsRGBA32().
	static const ImWchar icons_ranges[] = { 0x0100, 0x017E, 0 }; // Will not be copied by AddFont* so keep in scope.
	ImFontConfig config;
	config.MergeMode = true;
	config.OversampleH = 1;
	config.OversampleV = 1;
	config.GlyphOffset.y = 1;
	config.PixelSnapH = 1;
	config.GlyphExtraSpacing.x = 1.0f;
	//io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
	io.Fonts->AddFontFromFileTTF("fonts/inconsolata.otf", 13, &config, icons_ranges);
	io.Fonts->Build();

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	//ImGui::StyleColorsClassic();

	// Setup Platform/Renderer backends
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init(glsl_version);
	int logo_width = 0;
	int logo_height = 0;
	GLuint logo_texture = 0;
	bool ret = LoadTextureFromFile("img/ems_logo_grey.png", &logo_texture, &logo_width, &logo_height);
	int dual_width = 0;
	int dual_height = 0;
	GLuint dual_texture = 0;
	bool dual = LoadTextureFromFile("img/dual.png", &dual_texture, &dual_width, &dual_height);
	IM_ASSERT(dual);
	int security_width = 0;
	int security_height = 0;
	GLuint security_texture = 0;
	bool security = LoadTextureFromFile("img/security.png", &security_texture, &security_width, &security_height);
	IM_ASSERT(security);
	GLuint empty_texture = 0;
	bool empty = LoadTextureFromFile("img/empty.png", &empty_texture, &dual_width, &dual_height);
	IM_ASSERT(empty);
	int sounder_width = 0;
	int sounder_height = 0;
	GLuint sounder_texture = 0;
	bool sounder = LoadTextureFromFile("img/sounder.png", &sounder_texture, &sounder_width, &sounder_height);
	IM_ASSERT(sounder);
	int beacon_width = 0;
	int beacon_height = 0;
	GLuint beacon_texture = 0;
	bool beacon = LoadTextureFromFile("img/beacon.png", &beacon_texture, &beacon_width, &beacon_height);
	IM_ASSERT(beacon);
	int	unknown_width = 0;
	int unknown_height = 0;
	GLuint unknown_texture = 0;
	bool unknown = LoadTextureFromFile("img/unknown.png", &unknown_texture, &unknown_width, &unknown_height);
	IM_ASSERT(unknown);

	calibrationPresets presets[3];
	bool show_gui_log = false;
	bool update_sensor_data = false;
	bool skip_empty_sql = false;
	bool show_graph_window = false;
	bool show_cpk_window = false;
	bool save_smoke_to_csv = false;
	bool show_test_presets_window = false;
	bool show_tray_setup = false;
	bool engineerInterface = false;
	bool show_debug_window = true;
	ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
	char buffer[100];
	int time_ = 0;
	int time_sensors = 0; //resets each update cycle
	std::string engineerPassword = "8361";
	int alternate = 0; // update cycle splits the 32 sensors as handling all of them at once causes power draw issues
	int calibrationStatus = 0;
	std::string previousTest = "";
	std::vector<std::string> serialHeader; // used for smoke csv header
	loadTestPresets(presets);
	bool isLoggedIn = false;
	std::string logDate = getCurrentDateTime("date");
	bool tunnelPurged = false;
	bool fullPurgeRequired = true; //at startup or on tunnel disconnect
	int purgeStart = 0;
	int fullPurgeTime = 120; // time in seconds to purge on startup or tunnel disc
	bool skipPurge = false;
	bool tunnelEmptying = false;
	std::string currentTest = "";
	//main loop
	while (!glfwWindowShouldClose(window))
	{

		glfwPollEvents();
		time_++;
		time_sensors++;
		uptimeFrames++;
		{
			int failedSqlCount = 0;
			int sens = 0;//in the case that all detectors fail sql cancel the test
			for (int i = 0; i < SENSORS; i++) {
				if (sen[i].activated) {
					sens++;
					if (sen[i].testPassed == "MISSING SQL QUERIES"|| sen[i].testPassed=="PREVIOUS TEST NOT PASSED")
						failedSqlCount++;


					if (sen[i].testPassed == "REINSERT") {
						calibrationStatus = 16;
						break;
					}
				}
			}
			if (failedSqlCount == sens && sens != 0)
				calibrationStatus = 16;
		}
		static bool sentDoorOpenLog = false;
		if (calibrationStatus == 9 || calibrationStatus == 10 || calibrationStatus == 16 || calibrationStatus == 26
			|| calibrationStatus == 27 || fullPurgeRequired || tunnelReadings[6] == "0" || inComSetup) {
			if (tunnelReadings[6] == "0" && !sentDoorOpenLog) {
				LOG_F(0, "Door opened");
				sentDoorOpenLog = true;
			}
			bool testInProgress = false;
			for (int i = 0; i < SENSORS; i++)
				if (sen[i].completedTest == 0)
					testInProgress = true;
			if (testInProgress) {
				for (int i = 0; i < SENSORS; i++)
					sen[i].completedTest = 1;
				if (tunnelReadings[6] == "0") {
					for (int i = 0; i < SENSORS; i++)
						sen[i].testPassed = gettext("CANCELLED");

					calibrationStatus = 16;
				}
			}
		}
		if (tunnelReadings[6] == "1")
			if (sentDoorOpenLog)
				sentDoorOpenLog = false;

		//update cycle to get mb command
		if (time_sensors > (ImGui::GetIO().Framerate / (REFRESH_RATE * 2)) && run && tunnelReadings[6] == "1" && !inComSetup) { //tunnelReadings[6] = door closed
			std::thread worker_thread[SENSORS]; //handle each sensor for each thread
			for (int i = 0; i < SENSORS / 2; i++) {
				int n = -1;
				int senID = i + 16 * alternate;
				if (sen[senID].activated) { //add data points for graph
					sen[senID].sdata.AddPoint((float)tick + 1, (float)sen[i].SMOKE);
					sen[senID].sdata2.AddPoint((float)tick + 1, (float)sen[i].SMOKE / sen[senID].BASE);
				}
				if ((n = sen[senID].COM.find("COM")) != -1 && sen[senID].disconnectedCounter < 10 ) { //disconnectedCounter increments each consecutive update when it couldn't fetch sensor info
					bool MBCommandExists = false;
					for (int j = 0; j < sen[senID].queue.size(); j++)
						if (sen[senID].queue[j] == "MB")
							MBCommandExists = true;
					if (!MBCommandExists && (sen[senID].completedTest == 0 || update_sensor_data))
						sen[senID].queue.push_back("MB");
					worker_thread[senID] = std::thread(sendBytes, &(sen[senID]));
					worker_thread[senID].detach();
				}

			}
			alternate = !alternate;
			time_sensors = 0;
		}
		//update cycle for misc
		if (uptimeFrames > ImGui::GetIO().Framerate / REFRESH_RATE) {
			uptimeSeconds++;
			uptimeFrames = 0;
		}
		if (time_ > ImGui::GetIO().Framerate / REFRESH_RATE  && run && tunnelReadings[6] == "1" && !inComSetup) {

			if (!skipPurge && fullPurgeRequired && purgeStart + fullPurgeTime - 1 < uptimeSeconds) { // - 1 to sync with the update cycle correctly
				fullPurgeRequired = false;
				calibrationStatus = 0;
			}
			if (!tunnelPurged && ASK_LOGIN) {


				std::thread t = std::thread(stopCalibration, &calibrationStatus, &previousTest, &tunnelEmptying);
				t.detach();
				previousTest = currentTest;
				tunnelPurged = true;
			}


			if (logDate != getCurrentDateTime("date")) {
				loguru::remove_all_callbacks();
				std::string filePath = "log/log_" + getCurrentDateTime("date") + ".txt";
				logDate = getCurrentDateTime("date");
				loguru::add_file(filePath.c_str(), loguru::Append, loguru::Verbosity_MAX);

			}


			int completed = 0, inProgress = 0;
			for (int i = 0; i < SENSORS; i++) {
				if (sen[i].completedTest == 1)
					completed++;
				if (sen[i].completedTest == 0)
					inProgress++;
			}
			if (!inProgress && completed) {
				if (currentTest != "")
					previousTest = currentTest;
				currentTest = "";
			}

			if (!show_tray_setup)
				for (int j = 0; j < SENSORS; j++)
					COM_selectable[j] = sen[j].COM.c_str();
			int skip = 0;
			for (int k = 0; k < serialHeader.size(); k++)
				if (sen[k].serial == "") {
					skip++;
					continue;
				}
				else if (sen[k + skip].serial != serialHeader[k]) {
					serialHeaderChanged = true;
					break;
				}
			serialHeader.clear();

			if (save_smoke_to_csv) {
				std::thread smoke = std::thread(saveSmokeToCSV);
				smoke.detach();
			}
			time_ = 0;
			tick++;
			for (int i = 0; i < SENSORS; i++)
				if (sen[i].activated)
					serialHeader.push_back(sen[i].serial);


		}
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		if (engineerInterface) {
			static float f = 0.0f;
			static int counter = 0;
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->Pos);
			ImGui::SetNextWindowSize(ImVec2(200, 1000));
			ImGui::Begin("LEFT SIDE PANEL", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground
				| ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
			char versionbuff[40];
			snprintf(versionbuff, sizeof(versionbuff), "v%s", VERSION);
			ImGui::Text(versionbuff);
			ImGui::Image((void*)(intptr_t)logo_texture, ImVec2(logo_width * 0.135f, logo_height * 0.135f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
			ImGui::Text("[%s]", trayName[tray_1_ID].c_str());
			for (int i = 0; i < 32; i++) {
				if (i == 16) {
					ImGui::Text("");
					ImGui::Text("[%s]", trayName[tray_2_ID].c_str());
				}
				if (sen[i].activated && sen[i].fCTS) {
					ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(3 / 7.0f, 0.8f, 0.65f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(3 / 7.0f, 0.7f, 0.9f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(3 / 7.0f, 0.8f, 0.8f));
				}
				else if (sen[i].activated && !sen[i].fCTS) {
					ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(4 / 7.0f, 0.8f, 0.8f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(4 / 7.0f, 0.63f, 1.0f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(3.9 / 7.0f, 0.89f, 1.0f));
				}
				else if (!sen[i].activated && sen[i].fCTS) {
					ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.83f, 1.0f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.0f, 0.66f, 1.0f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0.0f, 0.9f, 0.90f));
				}
				else {
					ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.45f));
					ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.35f));
					ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(0.0f, 0.0f, 0.75f));
				}
				snprintf(buffer, 99, gettext("Sensor %d"), (i % 16) + 1);
				ImGui::Button(buffer, ImVec2(ImGui::GetWindowSize().x, 0.0f));
				ImGui::PopStyleColor(3);
			}
			ImGui::Text("");
			ImGui::Checkbox(gettext("Enable data refresh"), &update_sensor_data);
			ImGui::Checkbox(gettext("Save smoke data to CSV"), &save_smoke_to_csv);
			ImGui::Checkbox("Skip empty SQL queries", &skip_empty_sql);

			ImGui::End();


			{
				ImGui::SetNextWindowPos(ImVec2(120, 100));
				ImGui::SetNextWindowSize(ImVec2(1600, 900));
				ImGui::Begin("CENTER SENSORS", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
					ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
				if (ImGui::BeginTable("table1", 8))
				{
					int i = 0;
					for (int row = 0; row < 4; row++)
					{
						ImGui::TableNextRow();
						for (int column = 0; column < 8; column++)
						{
							ImGui::TableSetColumnIndex(column);
							if (sen[i].activated) {
								if (row == 2) {
									ImGui::Text("");
									ImGui::Text("");
								}
								ImGui::NextColumn();
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text(gettext("SENSOR %d"), (i % 16) + 1);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text("TT1: %d", sen[i].TT1);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text("TT2: %d", sen[i].TT2);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text("TT3: %d", sen[i].TT3);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text("TT4: %d", sen[i].TT4);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text("TT5: %d", sen[i].TT5);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text(gettext("SMOKE: %d"), sen[i].SMOKE);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								if (sen[i].SMOKE_STATUS == 2)
									ImGui::TextColored(ImVec4(1, 0, 0, 1), gettext("SMOKE STATUS: %d"), sen[i].SMOKE_STATUS);
								else if (sen[i].SMOKE_STATUS == 1)
									ImGui::TextColored(ImVec4(1, 0.5, 0, 1), gettext("SMOKE STATUS: %d"), sen[i].SMOKE_STATUS);
								else
									ImGui::TextColored(ImVec4(1, 1, 1, 1), gettext("SMOKE STATUS: %d"), sen[i].SMOKE_STATUS);
								ImGui::Text("");
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text("BASELINE: %d", sen[i].BASE);
								ImGui::Text("");//
								ImGui::SameLine(ImGui::GetColumnWidth() / 2);
								ImGui::Text("SERIAL: %s", sen[i].serial.c_str());
								ImGui::Text("");
							}
							else {
								if (row == 2) {
									ImGui::Text("");
									ImGui::Text("");
								}
								ImGui::Text("");
								ImGui::Text("");
								ImGui::Text("");
								ImGui::Text("");
								ImGui::Text(""); ImGui::SameLine(ImGui::GetColumnWidth() / 2); ImGui::Text(gettext("SENSOR %d"), (i % 16) + 1);
								ImGui::Text(""); ImGui::SameLine(ImGui::GetColumnWidth() / 2); ImGui::Text(gettext("[NO DATA]"));
								ImGui::Text("");
								ImGui::Text("");
								ImGui::Text("");
								ImGui::Text("");
							}
							i++;
						}
					}
					ImGui::EndTable();
				}

				ImGui::End();

			}
			{

				ImGui::SetNextWindowPos(ImVec2(1720, 0));
				ImGui::SetNextWindowSize(ImVec2(200, 1080));
				ImGui::Begin("RIGHT SIDE MENU", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
					ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
				if (ImGui::Button(gettext("Operator interface"), ImVec2(ImGui::GetWindowSize().x, 0.0f)))
					engineerInterface = !engineerInterface;
				ImGui::Text("");
				if (ImGui::Button(gettext("Lock door"), ImVec2(ImGui::GetWindowSize().x, 0.0f)))
					if (tunnelReadings[6] == "N/A")
						LOG_F(2, gettext("Tunnel not connected, could not lock door"));
					else
						tunnelCommands.push_back("LDR");
				if (ImGui::Button(gettext("Unlock door"), ImVec2(ImGui::GetWindowSize().x, 0.0f)))
					if (tunnelReadings[6] == "N/A")
						LOG_F(2, gettext("Tunnel not connected, could not unlock door"));
					else
						tunnelCommands.push_back("UDR");
				ImGui::Text("");
				if (ImGui::Button(gettext("Open ports"), ImVec2(ImGui::GetWindowSize().x, 0.0f)))
					for (int i = 0; i < SENSORS; i++) {
						openPorts(&sen[i]);
					}
				if (ImGui::Button(gettext("LED on"), ImVec2(ImGui::GetWindowSize().x, 0.0f))) {
					for (int i = 0; i < SENSORS; i++)
						if (sen[i].activated)
							if (sen[i].deviceType) {
								sen[i].queue.push_back("PORTD1111");
								sen[i].queue.push_back("PORTD2111");
							}
							else {
								sen[i].queue.push_back("PORTD1110");
								sen[i].queue.push_back("PORTD2110");
							}
				}
				if (ImGui::Button(gettext("LED off"), ImVec2(ImGui::GetWindowSize().x, 0.0f))) {
					for (int i = 0; i < SENSORS; i++)
						if (sen[i].activated)
							if (sen[i].deviceType) {
								sen[i].queue.push_back("PORTD1110");
								sen[i].queue.push_back("PORTD2110");
							}
							else {
								sen[i].queue.push_back("PORTD1111");
								sen[i].queue.push_back("PORTD2111");
							}
				}
				ImGui::Text("");
				if (ImGui::Button(gettext("Test configuration"), ImVec2(ImGui::GetWindowSize().x, 0.0f)))
					show_test_presets_window = !show_test_presets_window;
				if (ImGui::Button(gettext("Tray setup"), ImVec2(ImGui::GetWindowSize().x, 0.0f)))
					show_tray_setup = !show_tray_setup;
				if (ImGui::Button(gettext("Show graph"), ImVec2(ImGui::GetWindowSize().x, 0.0f)))
					show_graph_window = !show_graph_window;

				ImGui::Text("");
				ImGui::Text(gettext("Obscuration dB/m   %s"), tunnelReadings[0].c_str());
				ImGui::Text(gettext("Scatter dB/m       %s"), tunnelReadings[1].c_str());
				ImGui::Text(gettext("Temperature C      %s"), tunnelReadings[2].c_str());
				ImGui::Text(gettext("Motor RPM          %s"), tunnelReadings[3].c_str());
				ImGui::Text(gettext("Test number        %s"), tunnelReadings[4].c_str());
				ImGui::Text(gettext("Extract            %s"), tunnelReadings[5].c_str());
				ImGui::Text(gettext("Main Door closed   %s"), tunnelReadings[6].c_str());
				ImGui::Text(gettext("Main Door locked   %s"), tunnelReadings[7].c_str());
				ImGui::Text(gettext("Cal. relay output  %s"), tunnelReadings[8].c_str());
				ImGui::Text(gettext("Detector 1 Alarm   %s"), tunnelReadings[9].c_str());
				ImGui::Text(gettext("Detector 2 Alarm   %s"), tunnelReadings[10].c_str());
				ImGui::Text(gettext("Detector 3 Alarm   %s"), tunnelReadings[11].c_str());
				ImGui::Text(gettext("Detector 4 Alarm   %s"), tunnelReadings[12].c_str());
				ImGui::Text(gettext("Detector 5 Alarm   %s"), tunnelReadings[13].c_str());
				ImGui::Text(gettext("Detector 6 Alarm   %s"), tunnelReadings[14].c_str());
				ImGui::Text("(%.1f FPS)", ImGui::GetIO().Framerate);
				static int lang = 0;
				int lang_temp = lang;
				ImGui::RadioButton("English", &lang, 0);
				ImGui::RadioButton("Polskie", &lang, 1);

				if (lang != lang_temp)
					changeLanguage(lang);
				ImGui::End();
			}
			{

				guiLog.Draw(&show_gui_log);
			}
			if (show_test_presets_window) {
				int noPresets = 2;
				ImGui::SetNextWindowPos(ImVec2(1920 / 2, 1080 / 2), ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSize(ImVec2(350, 360), ImGuiCond_Always);
				ImGui::Begin(gettext("Test configuration"), &show_test_presets_window);
				static bool alreadyLoaded = false;
				char textbuff[100];
				static calibrationPresets presets_temp[3];


				static int presetNo = 1;

				//const char* items[] = { "Calibration", "Alarm clear", "Ramp alarm", "Alarm test"};
				const char* items[] = { "Calibration", "Alarm test" };
				if (!alreadyLoaded) {
					loadTestPresets(presets_temp);
					alreadyLoaded = true;
				}

				ImGui::SetNextItemWidth(200);
				ImGui::InputText(gettext("Test name"), &presets_temp[presetNo - 1].testName[0], 25);

				presets_temp[presetNo - 1].testName.resize(25);
				presets_temp[presetNo - 1].tunnelCommand.resize(25);

				if (presets_temp[presetNo - 1].testType == 0) {
					ImGui::SetNextItemWidth(200);
					ImGui::InputText(gettext("Tunnel command"), &presets_temp[presetNo - 1].tunnelCommand[0], 4);
				}
				else if (presets_temp[presetNo - 1].testType == 1) {
					ImGui::SetNextItemWidth(200);
					ImGui::InputText(gettext("SC tunnel command"), &presets_temp[presetNo - 1].tunnelCommand[0], 4);
					ImGui::SetNextItemWidth(200);
					ImGui::InputText(gettext("UTC tunnel command"), &presets_temp[presetNo - 1].tunnelCommand2[0], 4);

				}

				ImGui::RadioButton(gettext("Read from obscuration"), &presets_temp[presetNo - 1].readFromScatter, 0); ImGui::SameLine();
				ImGui::RadioButton(gettext("Read from scatter"), &presets_temp[presetNo - 1].readFromScatter, 1);

				ImGui::PushItemWidth(ImGui::GetWindowWidth() / 1.5);
				ImGui::Combo("Test Type", &presets_temp[presetNo - 1].testType, items, IM_ARRAYSIZE(items));
				ImGui::PopItemWidth();
				presets_temp[presetNo - 1].testType = presets_temp[presetNo - 1].testType;


				ImGui::SetNextItemWidth(100);

				if (presets_temp[presetNo - 1].testType == 0)
					ImGui::InputFloat(gettext("Minimum read value"), &presets_temp[presetNo - 1].min, 0.001f, 0.1f, "%.3f");
				else if (presets_temp[presetNo - 1].testType == 1)
					ImGui::InputFloat(gettext("Smartcell min read value"), &presets_temp[presetNo - 1].min_smartcell, 0.001f, 0.1f, "%.3f");

				ImGui::SetNextItemWidth(100);
				if (presets_temp[presetNo - 1].testType == 0)
					ImGui::InputFloat(gettext("Maximum read value"), &presets_temp[presetNo - 1].max, 0.001f, 0.1f, "%.3f");
				else if (presets_temp[presetNo - 1].testType == 1)
					ImGui::InputFloat(gettext("Smartcell max read value"), &presets_temp[presetNo - 1].max_smartcell, 0.001f, 0.1f, "%.3f");


				ImGui::SetNextItemWidth(100);

				if (presets_temp[presetNo - 1].testType == 1)
					ImGui::InputFloat("UTC min read value", &presets_temp[presetNo - 1].min_utc, 0.001f, 0.1f, "%.3f");
				else
					ImGui::InputInt(gettext("Samples"), &presets_temp[presetNo - 1].samples, 1, 5);

				ImGui::SetNextItemWidth(100);

				if (presets_temp[presetNo - 1].testType == 0)
					ImGui::InputFloat(gettext("Smartcell pre alarm set point"), &presets_temp[presetNo - 1].preAlarmSetPoint, 0.001f, 0.1f, "%.3f");
				else if (presets_temp[presetNo - 1].testType == 1)
					ImGui::InputFloat(gettext("UTC max read value"), &presets_temp[presetNo - 1].max_utc, 0.001f, 0.1f, "%.3f");


				ImGui::SetNextItemWidth(100);

				if (presets_temp[presetNo - 1].testType == 0)
					ImGui::InputFloat(gettext("Smartcell alarm set point"), &presets_temp[presetNo - 1].alarmSetPoint, 0.001f, 0.1f, "%.3f");
				else if (presets_temp[presetNo - 1].testType == 1)
					ImGui::InputInt(gettext("Samples"), &presets_temp[presetNo - 1].samples, 1, 5);


				ImGui::SetNextItemWidth(100);

				if (!presets_temp[presetNo - 1].testType)
					ImGui::InputFloat(gettext("UTC pre alarm set point"), &presets_temp[presetNo - 1].UTC_preAlarmSetPoint, 0.001f, 0.1f, "%.3f");

				ImGui::SetNextItemWidth(100);
				if (presets_temp[presetNo - 1].testType == 0) {
					ImGui::InputFloat(gettext("UTC alarm set point"), &presets_temp[presetNo - 1].UTC_alarmSetPoint, 0.001f, 0.1f, "%.3f");
				}

				if (ImGui::Button(gettext("<< Previous preset")))
					if (presetNo == 1)
						presetNo = noPresets;
					else
						presetNo--;

				ImGui::SameLine();
				ImGui::Text("  %d/2  ", presetNo); ImGui::SameLine();
				if (ImGui::Button(gettext("Next preset >>"), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
					if (presetNo == noPresets)
						presetNo = 1;
					else
						presetNo++;

				if (ImGui::Button(gettext("Load from file"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0)))
					if (loadTestPresets(presets_temp))
						LOG_F(0, gettext("Calibraton presets loaded from file"));

				ImGui::SameLine();

				if (ImGui::Button(gettext("Apply and save"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
					for (int i = 1; i < noPresets + 1; i++) {

						presets[i - 1].testName = presets_temp[i - 1].testName;
						presets[i - 1].tunnelCommand = presets_temp[i - 1].tunnelCommand;
						presets[i - 1].readFromScatter = presets_temp[i - 1].readFromScatter;
						presets[i - 1].testType = presets_temp[i - 1].testType;
						presets[i - 1].min = presets_temp[i - 1].min;
						presets[i - 1].max = presets_temp[i - 1].max;
						presets[i - 1].samples = presets_temp[i - 1].samples;
						presets[i - 1].preAlarmSetPoint = presets_temp[i - 1].preAlarmSetPoint;
						presets[i - 1].alarmSetPoint = presets_temp[i - 1].alarmSetPoint;
						presets[i - 1].UTC_preAlarmSetPoint = presets_temp[i - 1].UTC_preAlarmSetPoint;
						presets[i - 1].UTC_alarmSetPoint = presets_temp[i - 1].UTC_alarmSetPoint;
						presets[i - 1].id = presets_temp[i - 1].id;
						presets[i - 1].min_smartcell = presets_temp[i - 1].min_smartcell;
						presets[i - 1].max_smartcell = presets_temp[i - 1].max_smartcell;
						presets[i - 1].min_utc = presets_temp[i - 1].min_utc;
						presets[i - 1].max_utc = presets_temp[i - 1].max_utc;
						presets[i - 1].tunnelCommand2 = presets_temp[i - 1].tunnelCommand2;
					}
					saveCalibrationPresets(presets);

				}
				if (ImGui::Button(gettext("Reset file"), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
					ImGui::OpenPopup(gettext("Reset test presets"));
				if (ImGui::BeginPopup(gettext("Reset test presets"))) {
					ImGui::Text(gettext("Are you sure want to reset all test presets to the default values? \n\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("Yes"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) {
						if (createCalibrationPresets()) {
							LOG_F(0, gettext("Test presets reset to default"));
							alreadyLoaded = false;
						}
						else
							LOG_F(2, gettext("Could not reset Test presets file"));
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(gettext("No"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}



				ImGui::End();
			}

			if (show_graph_window) {
				ImGui::SetNextWindowPos(ImVec2(240, 250), ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowSize(ImVec2(1500, 950));
				ImGui::Begin(gettext("Graph Window"), &show_graph_window);

				static float history = 1000;
				static int r = 0;
				ImGui::Text("");
				ImGui::RadioButton(gettext("Smoke"), &r, 0); ImGui::SameLine();
				ImGui::RadioButton(gettext("Smoke/Baseline ratio"), &r, 1); ImGui::SameLine();
				ImGui::SetNextItemWidth(-60);
				ImGui::SliderFloat(gettext("History"), &history, 1, 1000, "%.1f s");
				static ImPlotAxisFlags flags = ImPlotAxisFlags_NoTickLabels;

				int i = 0;

				ImGui::Text("");
				if (ImGui::BeginTable("table_graph", 8))
				{
					for (int row = 0; row < 4; row++)
					{
						ImGui::TableNextRow();
						for (int column = 0; column < 8; column++)
						{
							ImGui::TableSetColumnIndex(column);
							ImGui::NextColumn();
							if (sen[i].activated && sen[i].sdata.Data.Size > 1) {


								ImPlot::PushColormap(ImPlotColormap_Deep);
								ImPlotStyle backup = ImPlot::GetStyle();
								StyleSeaborn();
								ImPlot::SetNextPlotLimitsX((double)tick - (double)history, tick, ImGuiCond_Always);

								TextCenter("Sensor %d smoke: %d", (sen[i].id % 16) + 1, sen[i].SMOKE);
								TextCenter("Smoke/Base ratio: %.2f", sen[i].SMOKE / (float)sen[i].BASE);
								snprintf(buffer, 99, "##Scrolling %d", i);
								if (!r) {
									ImPlot::SetNextPlotLimitsY(-100, 1000, ImGuiCond_Always);
									if (ImPlot::BeginPlot(buffer, NULL, NULL, ImVec2(175, 175), ImPlotFlags_AntiAliased, flags, flags)) {
										ImPlot::SetNextFillStyle(ImVec4((sen[i].SMOKE > (sen[i].BASE * 4.50f)) ? 1.0f : sen[i].SMOKE / (sen[i].BASE * 4.50f), 0.5f, 0.5f, 1.0f), 0.5f);
										ImPlot::PlotShaded("", &sen[i].sdata.Data[0].x, &sen[i].sdata.Data[0].y, sen[i].sdata.Data.size(), -INFINITY, sen[i].sdata.Offset, 8);
										ImPlot::PlotLine("", &sen[i].sdata.Data[0].x, &sen[i].sdata.Data[0].y, sen[i].sdata.Data.size(), sen[i].sdata.Offset, 8);
										ImPlot::EndPlot();
									}
								}
								else {
									ImPlot::SetNextPlotLimitsY(0, 10, ImGuiCond_Always);
									if (ImPlot::BeginPlot(buffer, NULL, NULL, ImVec2(175, 175), ImPlotFlags_AntiAliased, flags, flags)) {
										ImPlot::SetNextFillStyle(ImVec4((sen[i].SMOKE > (sen[i].BASE * 4.50f)) ? 1.0f : sen[i].SMOKE / (sen[i].BASE * 4.50f), 0.5f, 0.5f, 1.0f), 0.5f);
										ImPlot::PlotShaded("", &sen[i].sdata2.Data[0].x, &sen[i].sdata2.Data[0].y, sen[i].sdata2.Data.size(), -INFINITY, sen[i].sdata2.Offset, 8);
										ImPlot::PlotLine("", &sen[i].sdata2.Data[0].x, &sen[i].sdata2.Data[0].y, sen[i].sdata2.Data.size(), sen[i].sdata2.Offset, 8);
										ImPlot::EndPlot();
									}
								}
								ImPlot::GetStyle() = backup;
								ImPlot::PopColormap();

							}
							i++;

						}


					}
					ImGui::EndTable();
				}

				ImGui::End();
			}
			if (show_tray_setup)
			{
				ImGui::SetNextWindowSize(ImVec2(420, 880));
				ImGui::Begin(gettext("Tray setup"), &show_tray_setup);

				int drag_src = -1;
				int drag_dst = -1;
				static int tray_1_item = (tray_1_ID == 4) ? 0 : tray_1_ID;
				static int tray_2_item = (tray_2_ID == 4) ? 2 : tray_2_ID;
				const char* items[] = { "UTC_A", "UTC_B", "SMARTCELL_A", "SMARTCELL_B" };
				ImGui::PushItemWidth(ImGui::GetWindowWidth() / 2);
				ImGui::Combo("Tray 1", &tray_1_item, items, IM_ARRAYSIZE(items));
				ImGui::Combo("Tray 2", &tray_2_item, items, IM_ARRAYSIZE(items));
				ImGui::PopItemWidth();
				static int tray_1_item_previous = 0;
				static int tray_2_item_previous = 0;
				if (tray_1_item == tray_2_item) { //revert combo item to previous if trying to select the same two trays
					tray_1_item = tray_1_item_previous;
					tray_2_item = tray_2_item_previous;
				}
				else {
					tray_1_item_previous = tray_1_item;
					tray_2_item_previous = tray_2_item;
				}
				int n = 0;
				for (int i = 0; i < SENSORS / 2; i++)
					if (!strcmp(COM_selectable[i], ""))
						n++;
				if (n == SENSORS / 2)
					tray_1_ID = 4;
				n = 0;
				for (int i = SENSORS / 2; i < SENSORS; i++)
					if (!strcmp(COM_selectable[i], ""))
						n++;
				if (n == SENSORS / 2)
					tray_2_ID = 4;

				for (int n = 0; n < SENSORS; n++)
				{
					char bufferButton[100];
					sprintf_s(bufferButton, gettext("Ping Sensor %d [%s]"), (n % 16) + 1, n < 16 ? trayName[tray_1_ID].c_str() : trayName[tray_2_ID].c_str());
					const char* item = COM_selectable[n];
					if (sen[n].activated) {
						ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(3 / 7.0f, 0.8f, 0.65f));
						ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor::HSV(3 / 7.0f, 0.7f, 0.9f));
						ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor::HSV(3 / 7.0f, 0.8f, 0.8f));
					}
					if (ImGui::Button(bufferButton, ImVec2(ImGui::GetWindowSize().x / 2, 0.0f))) {
						for (int j = 0; j < SENSORS; j++)
							if (item == sen[j].COM) {
								if (sen[j].deviceType) {
									sen[j].queue.push_back("PORTD1111");
									sen[j].queue.push_back("PORTD1110");
									sen[j].queue.push_back("PORTD1111");
									sen[j].queue.push_back("PORTD1110");
								}
								else {
									sen[j].queue.push_back("PORTD1110");
									sen[j].queue.push_back("PORTD1111");
									sen[j].queue.push_back("PORTD1110");
									sen[j].queue.push_back("PORTD1111");
								}
							}
					}
					if (sen[n].activated)
						ImGui::PopStyleColor(3);
					ImGui::SameLine();
					ImGui::Selectable(item);
					if (ImGui::IsItemActive() && !ImGui::IsItemHovered())
					{
						int step = 0;
						if (ImGui::GetIO().MouseDelta.y < 0.0f && ImGui::GetMousePos().y < ImGui::GetItemRectMin().y)
							step = -1;
						if (ImGui::GetIO().MouseDelta.y > 0.0f && ImGui::GetMousePos().y > ImGui::GetItemRectMax().y)
							step = +1;
						if (step != 0)
						{
							drag_src = n;
							drag_dst = drag_src + step;
						}
					}
				}

				//// Move
				if (drag_dst >= 0 && drag_dst < SENSORS)
				{
					const char* b = COM_selectable[drag_src];
					COM_selectable[drag_src] = COM_selectable[drag_dst];
					COM_selectable[drag_dst] = b;
				}

				ImGui::Text("");
				if (ImGui::Button(gettext("Apply changes"), ImVec2(ImGui::GetContentRegionAvail().x / 3, 20))) {
					run = false;
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
					std::vector<std::string> temp;

					for (int i = 0; i < SENSORS; i++) {
						temp.push_back(COM_selectable[i]); //check
						sen[i].activated = false;
						sen[i].fCTS = false;
					}
					for (int i = 0; i < SENSORS; i++)
						sen[i].COM = "";
					for (int i = 0; i < SENSORS; i++) {
						sen[i].COM = temp[i];
						COM_selectable[i] = sen[i].COM.c_str();
					}
					serialHeaderChanged = true;
					run = true;
					tray_1_ID = tray_1_item;
					tray_2_ID = tray_2_item;
					LOG_F(0, gettext("Tray setup changes applied"));
				}
				ImGui::SameLine();
				if (ImGui::Button(gettext("Apply and save"), ImVec2(ImGui::GetContentRegionAvail().x / 2.25, 20))) {
					run = false;
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
					std::vector<std::string> temp;

					for (int i = 0; i < SENSORS; i++) {
						temp.push_back(COM_selectable[i]); //check
						sen[i].activated = false;
						sen[i].fCTS = false;
					}
					for (int i = 0; i < SENSORS; i++)
						sen[i].COM = "";
					for (int i = 0; i < SENSORS; i++) {
						sen[i].COM = temp[i];
						COM_selectable[i] = sen[i].COM.c_str();
					}
					serialHeaderChanged = true;
					run = true;
					tray_1_ID = tray_1_item;
					tray_2_ID = tray_2_item;


					std::ifstream file;
					std::ofstream file_temp;
					std::string tempLine;
					std::string line;
					file.open("config/COMs.txt");
					file_temp.open("config/COM_TMP.txt");
					int skip = 0;
					while (std::getline(file, line)) {
						//if (std::find(std::begin(items_s), std::end(items_s), line) != std::end(items_s)) {
						int t1 = 0;
						int t2 = 0;
						if ((t1 = line.find(items[tray_1_item])) != -1 || (t2 = line.find(items[tray_2_item])) != -1) {
							file_temp << line << '\n';
							for (int i = 0; i < SENSORS / 2; i++) {
								skip++;
								if (t1 != -1)
									tempLine = COM_selectable[i];
								else
									tempLine = COM_selectable[i + SENSORS / 2];
								int n = 0;
								if ((n = tempLine.find("COM")) != -1)
									file_temp << "\tsensor " << i + 1 << ":" << tempLine.substr(n, 6) << '\n';
								else
									file_temp << "\tsensor " << i + 1 << ":" << '\n';
							}
						}
						else if (skip)
							skip--;
						else
							file_temp << line << '\n';

					}
					file.close();
					file_temp.close();
					remove("config/COMs.txt");
					if (rename("config/COM_TMP.txt", "config/COMs.txt"))
						LOG_F(2, gettext("Error saving COMs.txt"));
					else
						LOG_F(0, gettext("Tray setup changes saved to config/COMs.txt"));

				}
				ImGui::SameLine();
				if (ImGui::Button(gettext("Load from COMs.txt"), ImVec2(ImGui::GetContentRegionAvail().x, 20)))
					COMSetup(false);
				if (ImGui::Button(gettext("Full COM port rescan"), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
					COMRescan();
				ImGui::End();
			}
		}
		else { // calibration interface
			int workingSenTray1 = 0;
			int workingSenTray2 = 0;
			static bool show_sensor_info = false;
			static bool tunnelClean = false;
			static std::string lastCleanDate = getCurrentDateTime("date");
			for (int i = 0; i < 16; i++)
				if (sen[i].activated)
					workingSenTray1++;
			for (int i = 16; i < 32; i++)
				if (sen[i].activated)
					workingSenTray2++;
			if (tunnelReadings[6] == "0")
				for (int i = 0; i < SENSORS; i++)
					sen[i].testPassed = "N/A";

			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(1920, 1080));
			static ImGuiTableFlags flags = ImGuiTableFlags_BordersOuterV | ImGuiTableFlags_BordersOuterH;
			static ImGuiTableFlags flagsTray = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInner;

			ImGui::Begin("calibration interface", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus);

			ImGui::Text(gettext("Control"));


			if (user.serial != "0000") {
				ImGui::SameLine(ImGui::GetContentRegionAvailWidth() - 200);
				ImGui::Text(gettext("Logged in as: %s %s"), user.firstName.c_str(), user.lastName.c_str());
			}
			if (ImGui::BeginTable("control", 3, flags)) {
				if (getCurrentDateTime("day") == "Monday" && getCurrentDateTime("hour") == "10"
					&& user.serial != "0000" && !tunnelClean && lastCleanDate != getCurrentDateTime("date")) {
					ImGui::OpenPopup("Recalibrate tunnel");
				}
				ImGui::TableSetupColumn("tests", ImGuiTableColumnFlags_WidthFixed, 1920 / 3);
				ImGui::TableSetupColumn("start stop buttons", ImGuiTableColumnFlags_WidthFixed, 1920 / 3);
				ImGui::TableSetupColumn("tunnel door control", ImGuiTableColumnFlags_WidthFixed, 1920 / 3);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);

				static int e = 0;
				if (!isLoggedIn)
					if (ASK_LOGIN)
						ImGui::OpenPopup("Log in");
				ImGui::Text("");
				ImGui::Text("");
				ImGui::Text(gettext("Select test to perform", l));
				if (!tunnelConnected)
					ImGui::PushDisabled();
				for (int i = 0; i < 2; i++)
					ImGui::RadioButton(presets[i].testName.c_str(), &e, i + 1);//check
				if (!tunnelConnected)
					ImGui::PopDisabled();
				ImGui::Text("");
				ImGui::Text("");
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("");
				ImGui::Text("");
				ImGui::Text("");
				if (calibrationStatus != 0 && calibrationStatus != 4 &&
					calibrationStatus != 5 && calibrationStatus != 7 &&
					calibrationStatus != 16 && //calibrationStatus &&
					calibrationStatus != 24 || fullPurgeRequired || inComSetup) {//check
					ImGui::PushDisabled();
					ImGui::Button(gettext("Start Calibration"), ImVec2(ImGui::GetContentRegionAvail().x / 3, 50));
					ImGui::PopDisabled();

				}
				else if (ImGui::Button(gettext("Start Calibration"), ImVec2(ImGui::GetContentRegionAvail().x / 3, 50)))
					startCalibration(e, &calibrationStatus, skip_empty_sql, &previousTest, &currentTest, &tunnelPurged);
				ImGui::SameLine();
				if (calibrationStatus != 1 && calibrationStatus != 2 &&
					calibrationStatus != 3 && calibrationStatus != 8 &&
					calibrationStatus != 15 && calibrationStatus != 20 &&
					calibrationStatus != 21 && calibrationStatus != 22 &&
					calibrationStatus != 23 || calibrationStatus == 10) {
					ImGui::PushDisabled();
					ImGui::Button(gettext("Cancel Calibration"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 50));
					ImGui::PopDisabled();
				}
				else if (ImGui::Button(gettext("Cancel Calibration"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 50)))
					ImGui::OpenPopup(gettext("Cancel calibration"));
				ImGui::SameLine();
				if (calibrationStatus == 10) {
					ImGui::PushDisabled();
					ImGui::Button(gettext("Empty Tunnel"), ImVec2(ImGui::GetContentRegionAvail().x, 50));
					ImGui::PopDisabled();
				}
				else if (ImGui::Button(gettext("Empty Tunnel"), ImVec2(ImGui::GetContentRegionAvail().x, 50))) {
					std::thread sc = std::thread(stopCalibration, &calibrationStatus, &previousTest, &tunnelEmptying);
					sc.detach();
				}


				ImVec2 center = ImGui::GetMainViewport()->GetCenter();
				ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

				if (ImGui::BeginPopupModal("Recalibrate tunnel", NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Tunnel must be cleaned and calibrated\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();
					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
						tunnelClean = true;
						lastCleanDate = getCurrentDateTime("date");
						ImGui::CloseCurrentPopup();
					}
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Tunnel not connected"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Calibration cannot start until the tunnel port is connected. If the usb is plugged into the PC try resetting the Lab Tunnel software\n\n"));
					ImGui::Separator();


					static bool dont_ask_me_next_time = false;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Tunnel temperature not in range"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Tunnel temperature must be between 20 to 30 (C) for accurate calibration\n\n"));
					ImGui::Separator();


					static bool dont_ask_me_next_time = false;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Already calibrating sensors"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Wait for the calibration to complete\n\n"));
					ImGui::Separator();


					static bool dont_ask_me_next_time = false;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("No sensors detected"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Please load sensors into tray\n\n"));
					ImGui::Separator();


					static bool dont_ask_me_next_time = false;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Tunnel must be emptied"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Tunnel must be clear before starting this test\n\n"));
					ImGui::Separator();


					static bool dont_ask_me_next_time = false;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Less than 30 sensors detected"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Are you sure all sensors have been connected? \n\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("Yes"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) { calibrationStatus = 8;  calibrateSensors(e, &calibrationStatus, skip_empty_sql); ImGui::CloseCurrentPopup(); } ImGui::SameLine();
					if (ImGui::Button(gettext("No"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}

				if (ImGui::BeginPopupModal(gettext("Not receiving communications"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Detected sensor(s) that aren't responding. Proceed anyway? \n\n"));
					ImGui::Separator();


					static bool dont_ask_me_next_time = false;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();
					if (ImGui::Button(gettext("Yes"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) { calibrationStatus = 8;  calibrateSensors(e, &calibrationStatus, skip_empty_sql);  ImGui::CloseCurrentPopup(); } ImGui::SameLine();
					if (ImGui::Button(gettext("No"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Database error"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Calibration could not start because it failed to connect \nto the SQL database or the parameters returned are empty.\nCheck log for more info \n\n"));
					ImGui::Separator();
					for (int i = 0; i < SENSORS; i++)
						if (sen[i].activated) {
							sen[i].testPassed = gettext("CANCELLED");
						}
					calibrationStatus = 16;
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();
					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); } ImGui::SameLine();
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Test not selected"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Please select a test to begin\n\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Door must be closed"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Please close tunnel door to start the test\n\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Different tray types"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("You cannot put Smartcell with UTC in the alarm test\n\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Calibration already in progress"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Please allow for sensors to finish calibration"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();

					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Cancel calibration"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					if (calibrationStatus == 3) {
						ImGui::Text(gettext("Could not cancel calibration as the alarm values are already being set"));
						ImGui::Separator();
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
						ImGui::PopStyleVar();
						if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
							ImGui::CloseCurrentPopup();
						}
						ImGui::SetItemDefaultFocus();
						ImGui::EndPopup();
					}
					else {

						ImGui::Text(gettext("Are you sure you want to cancel calibrating?"));
						ImGui::Separator();
						ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
						ImGui::PopStyleVar();


						if (ImGui::Button(gettext("Yes"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) {
							for (int i = 0; i < SENSORS; i++) {
								if (sen[i].activated) {
									sen[i].testPassed = gettext("CANCELLED");
								}
								sen[i].completedTest = 1;
							}
							LOG_F(1, gettext("User cancelled calibration"));  calibrationStatus = 16; ImGui::CloseCurrentPopup();
						} ImGui::SameLine();
						if (ImGui::Button(gettext("No"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
						ImGui::SetItemDefaultFocus();
						ImGui::EndPopup();
					}
				}
				if (calibrationStatus == 12) {
					ImGui::OpenPopup(gettext("Timeout reached"));
				}
				if (ImGui::BeginPopupModal(gettext("Timeout reached"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Timeout reached while trying to obtain tunnel and sensor data, check the min and max read range\nand make sure tunnel is completely closed\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();
					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { calibrationStatus = 0; ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (calibrationStatus == 13) {
					ImGui::OpenPopup(gettext("Invalid test preset"));
				}
				if (ImGui::BeginPopupModal(gettext("Invalid test preset"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text(gettext("Could not start test because the values in the preset were invalid, check log for more details\n"));
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();
					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { calibrationStatus = 0; ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}

				if (ImGui::BeginPopupModal(gettext("Calibration complete"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					static bool b = false;
					static int sensors = 0;
					static int success = 0;
					if (!b)
						for (int i = 0; i < SENSORS; i++)
							if (sen[i].activated) {
								sensors++;
								if (sen[i].testPassed == "PASSED")
									success++;
							}
					b = true;
					ImGui::Text(gettext("Calibrated %d/%d successfully\n\n"), success, sensors);
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();
					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
						ImGui::CloseCurrentPopup(); calibrationStatus = 4; tunnelCommands.push_back("UDR");
						b = false; sensors = 0; success = 0;
					}
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Alarm test complete"), NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					static bool b = false;
					static int failed = 0;
					static int success = 0;
					if (!b)
						for (int i = 0; i < SENSORS; i++)
							if (sen[i].activated) {
								if (sen[i].testPassed == "FAILED")
									failed++;
								if (sen[i].testPassed == "PASSED")
									success++;
							}
					b = true;
					ImGui::Text(gettext("%d/%d sensors passed alarm test successfully\n\n"), success, failed + success);
					ImGui::Separator();
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
					ImGui::PopStyleVar();
					if (ImGui::Button(gettext("OK"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
						ImGui::CloseCurrentPopup(); calibrationStatus = 24; tunnelCommands.push_back("UDR"); b = false; failed = 0; success = 0;
					}
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
				if (ImGui::BeginPopupModal(gettext("Log in"), NULL)) {

					//ImGui::SetNextWindowSize(ImVec2(100, 400), ImGuiCond_Always);
					ImGui::Text(gettext("Please enter or scan your user ID \n\n"));
					static char serial[64] = "";

					ImGui::PushItemWidth(ImGui::GetWindowWidth());
					if (!ImGui::IsAnyItemActive())
						ImGui::SetKeyboardFocusHere();
					static int temp = 0;
					temp = strlen(serial);
					ImGui::InputText("", serial, 64, ImGuiInputTextFlags_CharsNoBlank);
					ImGui::PopItemWidth();
					ImGui::Separator();

					if (strlen(serial) > temp + 1 || ImGui::IsKeyPressedMap(ImGuiKey_Enter) || ImGui::IsKeyPressedMap(ImGuiKey_KeyPadEnter) || ImGui::Button(gettext("Log in"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
						std::vector<std::string> results;
						snprintf(buffer, sizeof(buffer), "SELECT  * from employees.users where user_id = %s", serial);
						//char buftemp[5000];
						//snprintf(buftemp, sizeof(buftemp), "SELECT 	CALIBRATION_BASELINE, CALIBRATION_CHAMBER_POSITION, \
			CALIBRATION_OBSCURATION_VALUE, CALIBRATION_RESULT, CALIBRATION_SMOKE_VALUE, CALIBRATION_TEMPERATURE, DARK_REF, DARK_VALUE, \
			DateTime_of_Test, Idle_Time, test_time, LIGHT_REF, LIGHT_VALUE from tracking_smartcell.CALIBRATION_TEST_STAGE where assembled_ident_number = '%s'",serial);
						int sqlStatus = 0;
						results = ExecuteSql(buffer, &sqlStatus);
						if (results.size()) {
							//std::string conc = "";
							//for (int j = 0; j < results.size(); j += 13) {
							//	for (int i = 0; i < 13; i++)
							//	{
							//		conc += results[i + j];
							//		conc += " ";
							//	}
							//	LOG_F(0, "%s", conc.c_str());
							//	conc = "";
							//}
							ImGui::CloseCurrentPopup();
							isLoggedIn = true;
							user.serial = results[0];
							user.firstName = results[2];
							user.lastName = results[3];
							LOG_F(0, "User %s (%s %s) logged in", user.serial.c_str(), user.firstName.c_str(), user.lastName.c_str());
							for (int i = 0; i < 64; i++)
								serial[i] = '\0'; //clear text input field
						}
					}
					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Log out"), NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
					ImGui::SetNextWindowPos(ImVec2(0, 0));
					ImGui::SetNextWindowSize(ImVec2(200, 200));
					ImGui::Text(gettext("Are you sure you want to log out?\n\n"));


					if (ImGui::Button(gettext("Yes"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) {
						ImGui::CloseCurrentPopup();
						LOG_F(0, gettext("User %d logged out", users[currentUserID].serial));
						user.firstName = "null";
						user.lastName = "null";
						user.serial = "0000";
						isLoggedIn = false;
					}
					ImGui::SameLine();
					if (ImGui::Button(gettext("No"), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
						ImGui::CloseCurrentPopup();

					ImGui::EndPopup();
				}
				if (ImGui::BeginPopupModal(gettext("Password required"), NULL, ImGuiWindowFlags_NoResize)) {
					ImGui::SetNextWindowPos(ImVec2(0, 0));
					ImGui::Text(gettext("Please enter the password to unlock tunnel door while calibrating \n\n"));

					static char bufpass[64] = "";
					if (!ImGui::IsAnyItemActive())
						ImGui::SetKeyboardFocusHere();
					ImGui::PushItemWidth(ImGui::GetWindowWidth());
					ImGui::InputText("", bufpass, 64, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsNoBlank);
					ImGui::PopItemWidth();
					ImGui::Separator();
					if (ImGui::IsKeyPressedMap(ImGuiKey_Enter) || ImGui::IsKeyPressedMap(ImGuiKey_KeyPadEnter) || ImGui::Button(gettext("Unlock door"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) {
						if (!strcmp(bufpass, engineerPassword.c_str())) {
							tunnelCommands.push_back("UDR");
							LOG_F(0, "Unlocked door while calibrating");
							for (int i = 0; i < 64; i++)
								bufpass[i] = '\0'; //clear text input field
						}
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(gettext("Cancel"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}

				if (ImGui::BeginPopupModal(gettext("Engineer debug interface"), NULL, ImGuiWindowFlags_NoResize)) {
					ImGui::SetNextWindowPos(ImVec2(0, 0));
					ImGui::Text(gettext("Please enter the password to access the engineer debug interface \n\n"));

					static char bufpass[64] = "";
					if (!ImGui::IsAnyItemActive())
						ImGui::SetKeyboardFocusHere();
					ImGui::PushItemWidth(ImGui::GetWindowWidth());
					ImGui::InputText("", bufpass, 64, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsNoBlank);
					ImGui::PopItemWidth();
					ImGui::Separator();
					if (ImGui::IsKeyPressedMap(ImGuiKey_Enter) || ImGui::IsKeyPressedMap(ImGuiKey_KeyPadEnter) || ImGui::Button(gettext("Log in"), ImVec2(ImGui::GetContentRegionAvail().x / 2, 0))) {
						if (!strcmp(bufpass, engineerPassword.c_str())) {
							engineerInterface = !engineerInterface;
							LOG_F(0, "Engineer debug interface accessed");
							for (int i = 0; i < 64; i++)
								bufpass[i] = '\0'; //clear text input field
						}
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button(gettext("Cancel"), ImVec2(ImGui::GetContentRegionAvail().x, 0))) { ImGui::CloseCurrentPopup(); }
					ImGui::SetItemDefaultFocus();
					ImGui::EndPopup();
				}
				ImGui::Text("");
				ImGui::Text("");
				if (!tunnelConnected)
					calibrationStatus = 10;
				else if (fullPurgeRequired)
					calibrationStatus = 27;
				else if (tunnelEmptying)
					calibrationStatus = 9;
				switch (calibrationStatus) {

				case 0: TextCenter(gettext("Status: Ready"));
					break;
				case 1: char statusbuff[256];
					TextCenter(gettext("Status: Waiting for tunnel to reach targeted obscuration/scatter level"));
					snprintf(statusbuff, 256, gettext("Currently at %s"), tunnelReadings[1].c_str());
					TextCenter(statusbuff);
					break;
				case 2: TextCenter(gettext("Status: Getting smoke average from sensors"));
					break;
				case 3: TextCenter(gettext("Status: Setting pre alarm and alarm values"));
					break;
				case 4: TextCenter(gettext("Status: Calibration complete, ready"));
					break;
				case 5: TextCenter(gettext("Status: Calibration complete"));
					ImGui::OpenPopup(gettext("Calibration complete"));
					calibrationStatus = 4;
					break;
				case 6: TextCenter("Status: Tunnel in test but currently not calibrating");
					break;
				case 7: TextCenter("Status: Possible obscuration meter fault");
					break;
				case 8: TextCenter("Status: Preparing to calibrate");
					break;
				case 9:
					snprintf(statusbuff, 256, gettext("Status: Clearing tunnel (%s)"), tunnelReadings[1].c_str());
					TextCenter(statusbuff);
					if (tunnelReadings[4] == "00" && !tunnelEmptying)
						calibrationStatus = 0;
					break;
				case 10: if (tunnelConnected)
					calibrationStatus = 0;
					   else
					TextCenter(gettext("Status: Tunnel not connected"));
					fullPurgeRequired = true;
					purgeStart = uptimeSeconds;
					tunnelPurged = false;
					break;
				case 11: TextCenter(gettext("Status: Invalid test presets"));
					break;
				case 12: TextCenter(gettext("Status: Timeout reached"));
					break;
				case 15: TextCenter(gettext("Status: Opening up ports"));
					break;
				case 16: TextCenter(gettext("Status: Cancelled calibration/alarm test"));
					if (tunnelReadings[6] == "0")
						calibrationStatus = 0;
					break;
				case 20: TextCenter(gettext("Status: Waiting for tunnel to reach scatter level for the alarm clear stage"));
					snprintf(statusbuff, 256, gettext("Currently at %sdB/m"), tunnelReadings[1].c_str());
					TextCenter(statusbuff);
					break;
				case 21: TextCenter(gettext("Status: Testing alarm clear"));
					break;
				case 22: TextCenter(gettext("Status: Waiting for tunnel to reach scatter level for the alarm stage"));
					snprintf(statusbuff, 256, gettext("Currently at %sdB/m"), tunnelReadings[1].c_str());
					TextCenter(statusbuff);
					break;
				case 23: TextCenter(gettext("Status: Testing alarm"));
					break;
				case 24: TextCenter(gettext("Status: Completed alarm test, ready"));
					break;
				case 25: TextCenter(gettext("Status: Completed alarm test"));
					ImGui::OpenPopup("Alarm test complete");
					calibrationStatus = 24;
					break;
				case 26: TextCenter(gettext("Status: Database error"));
					ImGui::OpenPopup(gettext("Database error"));
					if (tunnelReadings[6] == "0")
						calibrationStatus = 0;
					break;
				case 27: snprintf(statusbuff, 256, gettext("Clearing tunnel (%ds)"), (fullPurgeTime + purgeStart - uptimeSeconds));
					TextCenter(statusbuff);
					break;
				}

				if (skip_empty_sql) {
					TextCenterColored(ImVec4(1, 0.5, 0, 1), gettext("Overriding missing SQL queries"));
					TextCenterColored(ImVec4(1, 0.5, 0, 1), gettext("Will not write to database as pass"));
				}

				ImGui::TableSetColumnIndex(2);
				ImGui::Text("");

				ImGui::Text(""); ImGui::SameLine(ImGui::GetContentRegionAvailWidth() / 2);

				if (ImGui::Button(gettext("Engineer debug interface"), ImVec2(ImGui::GetContentRegionAvail().x, 30)))
					if (ASK_LOGIN)
						ImGui::OpenPopup("Engineer debug interface");
					else
						engineerInterface = !engineerInterface;



				ImGui::Text(""); ImGui::SameLine(ImGui::GetContentRegionAvailWidth() / 2);
				if (calibrationStatus == 10) {
					ImGui::PushDisabled();
					ImGui::Button(gettext("Unlock door"), ImVec2(ImGui::GetContentRegionAvail().x, 30));
					ImGui::PopDisabled();
				}
				else if (ImGui::Button(gettext("Unlock door"), ImVec2(ImGui::GetContentRegionAvail().x, 30)))
					if (calibrationStatus != 0 && calibrationStatus != 4 && calibrationStatus != 5 && calibrationStatus != 12
						&& calibrationStatus != 9 && calibrationStatus != 16 && calibrationStatus != 11
						&& calibrationStatus != 24 && calibrationStatus != 25)
						ImGui::OpenPopup("Password required");

					else
						tunnelCommands.push_back("UDR");
				ImGui::Text(""); ImGui::SameLine(ImGui::GetContentRegionAvailWidth() / 2);
				if (calibrationStatus == 10) {
					ImGui::PushDisabled();
					ImGui::Button(gettext("Lock door"), ImVec2(ImGui::GetContentRegionAvail().x, 30));
					ImGui::PopDisabled();
				}
				else if (ImGui::Button(gettext("Lock door"), ImVec2(ImGui::GetContentRegionAvail().x, 30)))
					tunnelCommands.push_back("LDR");

				ImGui::Text(""); ImGui::SameLine(ImGui::GetContentRegionAvailWidth() / 2);
				if (ImGui::Button(gettext("Show sensor info"), ImVec2(ImGui::GetContentRegionAvail().x, 30)))
					show_sensor_info = !show_sensor_info;

				ImGui::Text(""); ImGui::SameLine(ImGui::GetContentRegionAvailWidth() / 2);
				if (ImGui::Button(gettext("Log out"), ImVec2(ImGui::GetContentRegionAvail().x, 30)))
					ImGui::OpenPopup("Log out");


				ImGui::Text("");

				ImGui::EndTable();
			}

			ImGui::Text(gettext("[%s]: %d/16 sensors connected"), trayName[tray_1_ID].c_str(), workingSenTray1);
			if (ImGui::BeginTable("tray1", 8, flagsTray))
			{
				int i = 0;
				ImU32 cell_bg_color_success = ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.1f, 0.65f));
				ImU32 cell_bg_color_warn = ImGui::GetColorU32(ImVec4(1.0f, 0.75f, 0.0f, 0.80f));
				ImU32 cell_bg_color_fail = ImGui::GetColorU32(ImVec4(0.8f, 0.2f, 0.1f, 0.65f));
				ImU32 cell_bg_color_grey = ImGui::GetColorU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
				ImU32 cell_bg_color_header = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.f));

				if (show_sensor_info)
					for (int j = 0; j < 8; j++)
						ImGui::TableSetupColumn("TRAY 1", ImGuiTableColumnFlags_WidthFixed, 230.0f);
				else
					for (int j = 0; j < 8; j++)
						ImGui::TableSetupColumn("TRAY 1");
				for (int row = 0; row < 2; row++)
				{
					ImGui::TableNextRow();
					for (int column = 0; column < 8; column++)
					{
						ImGui::TableSetColumnIndex(column);
						if (!show_sensor_info) {
							char buff[10];
							snprintf(buff, 10, "%d", (sen[i].id % 16) + 1);
							ImGui::Text(buff);
							ImGui::Text(""); ImGui::SameLine(45);
							if (sen[i].serial != "") {
								if (calibrationStatus == 24) {
									if (sen[i].testPassed == gettext("PASSED"))
										ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								}
								if (sen[i].testPassed == gettext("PASSED"))
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else if (sen[i].testPassed == "PREVIOUS TEST NOT PASSED") {
									ImGui::Text("Previous test not passed");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								}
								else if (sen[i].testPassed == "MISSING SQL QUERIES") {
									ImGui::Text("SQL failed");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_warn);
								}
								else if (sen[i].testPassed == "REINSERT") {
									ImGui::Text("Reinsert sensor");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_warn);
								}
								else if (sen[i].testPassed == "SQL INSERT FAILED") {
									ImGui::Text("SQL insert failed");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								}
								else if (sen[i].testPassed != "N/A")
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								if (sen[i].deviceType == 0)
									ImGui::Image((void*)(intptr_t)security_texture, ImVec2(security_width * 0.17f, security_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].deviceType == 1)
									ImGui::Image((void*)(intptr_t)dual_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].deviceType == 2)
									ImGui::Image((void*)(intptr_t)sounder_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].deviceType == 3)
									ImGui::Image((void*)(intptr_t)beacon_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].deviceType == 4)
									ImGui::Image((void*)(intptr_t)unknown_texture, ImVec2(unknown_width * 0.17f, unknown_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].fCTS) {
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								}

							}
							else if (sen[i].serial == "") {
								if (sen[i].faultyCOM) {
									ImGui::Text("COM port disabled");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);

								}
								if (sen[i].fCTS) {
									ImGui::Text("Sensor detected but not\n receiving communications");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);

								}
								else
									ImGui::Image((void*)(intptr_t)empty_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
							}
						}
						else {
							if (ImGui::BeginTable("tray1Contents", 1, ImGuiTableFlags_Resizable)) {
								ImGui::TableSetupColumn(0);

								ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing());
								ImGui::TableNextColumn();
								ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_header);
								ImGui::Text("SENSOR %d", (sen[i].id % 16) + 1);
								ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing());
								ImGui::TableNextColumn();
								if (sen[i].fCTS)
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_grey);
								ImGui::Text(gettext("PLACED: %s"), sen[i].fCTS ? "YES" : "NO");
								ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing());
								ImGui::TableNextColumn();
								if (sen[i].faultyCOM == false)
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								ImGui::Text("COM PORT: %s", sen[i].COM.c_str());
								ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing());
								ImGui::TableNextColumn();
								if (sen[i].testPassed == gettext("PASSED"))
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else if (sen[i].testPassed == "N/A")
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_grey);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								ImGui::Text(gettext("TEST STATUS: %s"), sen[i].testPassed.c_str());
								ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetTextLineHeightWithSpacing());
								ImGui::TableNextColumn();
								if (sen[i].serial == "" || sen[i].serial == "NULL")
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_grey);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								ImGui::Text(gettext("SERIAL: %s"), sen[i].serial.c_str());

								ImGui::EndTable();
							}

						}
						i++;
					}
				}
				ImGui::EndTable();
			}
			ImGui::Text(gettext("[%s]: %d/16 sensors connected"), trayName[tray_2_ID].c_str(), workingSenTray2);
			if (ImGui::BeginTable("tray2", 8, flagsTray))
			{
				int i = 16;
				ImU32 cell_bg_color_success = ImGui::GetColorU32(ImVec4(0.3f, 0.8f, 0.1f, 0.65f));
				ImU32 cell_bg_color_warn = ImGui::GetColorU32(ImVec4(1.0f, 0.75f, 0.0f, 0.80f));
				ImU32 cell_bg_color_fail = ImGui::GetColorU32(ImVec4(0.8f, 0.2f, 0.1f, 0.65f));
				ImU32 cell_bg_color_grey = ImGui::GetColorU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
				if (show_sensor_info)
					for (int j = 0; j < 8; j++)
						ImGui::TableSetupColumn("TRAY 2", ImGuiTableColumnFlags_WidthFixed, 230.0f);
				else
					ImGui::TableSetupColumn("TRAY 2");

				for (int row = 0; row < 2; row++)
				{
					ImGui::TableNextRow();
					for (int column = 0; column < 8; column++)
					{
						ImGui::TableSetColumnIndex(column);
						if (!show_sensor_info) {
							char buff[10];
							snprintf(buff, 10, "%d", (sen[i].id % 16) + 1);
							ImGui::Text(buff);
							ImGui::Text(""); ImGui::SameLine(45);
							if (sen[i].serial != "") {
								if (sen[i].testPassed == gettext("PASSED"))
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else if (sen[i].testPassed == "PREVIOUS TEST NOT PASSED") {
									ImGui::Text("Previous test not passed");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								}
								else if (sen[i].testPassed == "MISSING SQL QUERIES") {
									ImGui::Text("SQL failed");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_warn);
								}
								else if (sen[i].testPassed == "REINSERT") {
									ImGui::Text("Reinsert sensor");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_warn);
								}
								else if (sen[i].testPassed == "SQL INSERT FAILED") {
									ImGui::Text("SQL insert failed");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								}
								else if (sen[i].testPassed != "N/A")
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);

								if (sen[i].deviceType == 0)
									ImGui::Image((void*)(intptr_t)security_texture, ImVec2(security_width * 0.17f, security_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].deviceType == 1)
									ImGui::Image((void*)(intptr_t)dual_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].deviceType == 2)
									ImGui::Image((void*)(intptr_t)sounder_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
								else if (sen[i].deviceType == 3)
									ImGui::Image((void*)(intptr_t)beacon_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));

							}
							else if (sen[i].serial == "") {
								if (sen[i].faultyCOM) {
									ImGui::Text("COM port disabled");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);

								}
								if (sen[i].fCTS) {
									ImGui::Text("Sensor detected but not \nreceiving communications");
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);

								}
								else
									ImGui::Image((void*)(intptr_t)empty_texture, ImVec2(dual_width * 0.17f, dual_height * 0.17f), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
							}
						}
						else {
							if (ImGui::BeginTable("tray2Contents", 1, ImGuiTableFlags_Resizable)) {
								ImGui::TableSetupColumn(0);

								ImGui::TableNextRow(ImGuiTableRowFlags_None);
								ImGui::TableNextColumn();

								ImGui::Text("SENSOR %d", (i % 16) + 1);
								ImGui::TableNextRow(ImGuiTableRowFlags_None);
								ImGui::TableNextColumn();
								if (sen[i].fCTS)
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_grey);
								ImGui::Text("PLACED: %s", sen[i].fCTS ? "YES" : "NO");
								ImGui::TableNextRow(ImGuiTableRowFlags_None);
								ImGui::TableNextColumn();
								if (sen[i].faultyCOM == false)
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								ImGui::Text("COM PORT: %s", sen[i].COM.c_str());
								ImGui::TableNextRow(ImGuiTableRowFlags_None);
								ImGui::TableNextColumn();
								if (sen[i].testPassed == "PASSED")
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								else if (sen[i].testPassed == "N/A")
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_grey);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_fail);
								ImGui::Text("TEST STATUS: %s", sen[i].testPassed.c_str());
								ImGui::TableNextRow(ImGuiTableRowFlags_None);
								ImGui::TableNextColumn();
								if (sen[i].serial == "" || sen[i].serial == "NULL")
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_grey);
								else
									ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, cell_bg_color_success);
								ImGui::Text("SERIAL: %s", sen[i].serial.c_str());

								ImGui::EndTable();
							}
						}
						i++;
					}
				}

				ImGui::EndTable();
			}

			ImGui::End();

		}

		// Rendering
		ImGui::Render();
		int display_w, display_h;
		glfwGetFramebufferSize(window, &display_w, &display_h);
		glViewport(0, 0, display_w, display_h);
		glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glfwSwapBuffers(window);
	}

	// Cleanup
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImPlot::DestroyContext();
	ImGui::DestroyContext();

	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}
//=============================================================================================================//
int sendBytes(sensorStruct* sensor) {
	std::string command;
	if (sensor->COM == tunnelCOM)
		return 0;
	if (sensor->queue.empty() && sensor->serial != "" && sensor->deviceType != 4 && !sensor->sendRTS)
		return 0;
	if (sensor->COM == "")
		return 0;
	//if (sensor->disconnectedCounter > 10)
	//	return 0;
	if (!sensor->isClosed)
		return 0;
	sensor->isClosed = false;



	if (command == "PORTG1111")
		std::this_thread::sleep_for(std::chrono::seconds(5));//supercap needs to charge prior to opening this port command

	sensor->hSerial = CreateFileA(sensor->COM.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0,    // must be opened with exclusive-access
		NULL, // no security attributes
		OPEN_EXISTING, // must use OPEN_EXISTING
		0,    // not overlapped I/O
		NULL  // hTemplate must be NULL for comm devices
	);
	DCB dcbSerialParams = { 0 };
	COMMTIMEOUTS timeouts = { 0 };
	OVERLAPPED ol = { 0 };
	dcbSerialParams.DCBlength = sizeof(dcbSerialParams);

	if (sensor->hSerial == INVALID_HANDLE_VALUE || GetLastError() == 5 || !GetCommState(sensor->hSerial, &dcbSerialParams)) {
		if (sensor->disconnectedCounter < 2)
			LOG_F(2, gettext("Failed opening %s with command %s"), sensor->COM.c_str(), command.c_str());
		closeHandleWrapper(sensor);
		sensor->faultyCOM = true;
		return 0;
	}
	sensor->faultyCOM = false;
	dcbSerialParams.BaudRate = CBR_57600;
	dcbSerialParams.ByteSize = 8;
	dcbSerialParams.StopBits = ONESTOPBIT;
	dcbSerialParams.Parity = NOPARITY;
	dcbSerialParams.fDtrControl = 0;
	dcbSerialParams.fRtsControl = 0;
	dcbSerialParams.fOutxDsrFlow = 0;
	dcbSerialParams.fOutxCtsFlow = 0;
	if (!SetCommState(sensor->hSerial, &dcbSerialParams))  //Configuring the port according to settings in DCB
	{
		LOG_F(2, "Error in SetCommState()");
		closeHandleWrapper(sensor);
		return 1;
	}
	DWORD dwModemStatus;


	if (!GetCommModemStatus(sensor->hSerial, &dwModemStatus)) {
		closeHandleWrapper(sensor);
		return 0;
	}

	sensor->fCTS = MS_CTS_ON & dwModemStatus;


	if (!(sensor->fCTS)) {
		closeHandleWrapper(sensor);
		return 0;
	}

	timeouts.ReadIntervalTimeout = 0;
	timeouts.ReadTotalTimeoutConstant = 500;
	timeouts.ReadTotalTimeoutMultiplier = 0;
	timeouts.WriteTotalTimeoutConstant = 50;
	timeouts.WriteTotalTimeoutMultiplier = 10;

	if (SetCommTimeouts(sensor->hSerial, &timeouts) == 0)
	{
		LOG_F(2, "Error setting serial port timeouts");
		closeHandleWrapper(sensor);
		return 0;
	}

	if (sensor->sendRTS) {
		sensor->sendRTS = false;
		std::this_thread::sleep_for(std::chrono::seconds(3));
		EscapeCommFunction(sensor->hSerial, SETRTS);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		EscapeCommFunction(sensor->hSerial, CLRRTS);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		if (sensor->testPassed == "REINSERT") {
			sensor->testPassed = "N/A";
			if (sensor->queue.empty())
				sensor->queue.clear();
			openPorts(sensor);
		}
		closeHandleWrapper(sensor);
		return 0;
	}

	if (sensor->serial == "")
		command = "IR";
	else if (sensor->deviceType == 4)
		command = "DR";
	else
		command = sensor->queue.front();

	///*----------------------------- Writing a Character to Serial Port----------------------------------------*/
	int length = (int)command.length();
	std::string cmdToSend = command + "\r\n";
	DWORD  dNoOFBytestoWrite;              // No of bytes to write into the port
	DWORD  dNoOfBytesWritten = 0;          // No of bytes written to the port
	dNoOFBytestoWrite = length + 2; // Calculating the no of bytes to write into the port

	if (!WriteFile(sensor->hSerial, cmdToSend.c_str(), dNoOFBytestoWrite, &dNoOfBytesWritten, NULL)) {
		LOG_F(2, "Error sending bytes to %s", sensor->COM.c_str());
		closeHandleWrapper(sensor);
		return 0;
	}

	int p = 0;
	int maxChars = 512;
	BOOL  Read_Status;                      // Status of the various operations 
	char  SerialBuffer;               // Buffer Containing Rxed Data
	for (int i = 0; i < maxChars; i++)
		sensor->receive[i] = 0;
	DWORD NoBytesRead;                     // Bytes read by ReadFile()


	///*------------------------------------ Setting Receive Mask ----------------------------------------------*/
	Read_Status = SetCommMask(sensor->hSerial, EV_RXCHAR); //Configure Windows to Monitor the serial device for Character Reception
	do {
		if (ReadFile(sensor->hSerial, &SerialBuffer, 1, &NoBytesRead, NULL))
			sensor->receive[p] = SerialBuffer;
		p++;

		if (SerialBuffer == '\r' && (command != "MR" && command != "PORTB0110"))
			break;
	} while (NoBytesRead);
	if (!sensor->receive[1] || GetLastError() == 6) {//|| !(sensor->fCTS)) {
		if (sensor->fCTS)
			sensor->sendRTS = true;
		closeHandleWrapper(sensor);
		return 0;
	}

	if (Read_Status == FALSE) {
		LOG_F(2, "Error setting CommMask");
		closeHandleWrapper(sensor);
		return 0;
	}
	int n = 0;
	char split[512];
	int splitChar = 0;
	std::string splitSend;

	if (command == "PORTB0110") {
		std::string rcv = sensor->receive;
		if (rcv.find("BROWNOUT RESET") != -1) {
			sensor->testPassed = "REINSERT";
			sensor->sendRTS = true;
			closeHandleWrapper(sensor);
			return 0;

		}
	}
	if (command != "IR" && command != "DR" && command != "MB" && command != "PORTD1110")
		while (sensor->receive[n] != '\0' && n < maxChars) {
			if (sensor->receive[n] == '\r') {
				split[splitChar] = '\0';
				LOG_F(0, "[%s] %s", sensor->serial.c_str(), split);
				for (int i = 0; i < maxChars; i++)
					split[i] = '\0';
				splitChar = 0;
				n++;
			}
			else if (sensor->receive[n] == '\n') {

				n++;
				continue;
			}
			else
				split[splitChar++] = sensor->receive[n++];

		}
	sensor->disconnectedCounter = 0;
	sensor->receive[p++] = '\0';
	sensor->activated = 1;
	if (command == "MR") {
		sensor->mrReceive = sensor->receive;
	}
	int commaCounter = 0;
	if (command == "IR") {
		if (sensor->receive[0] != 'I' || sensor->receive[1] != ' ' || sensor->receive[2] != 'R') {
			LOG_F(2, gettext("Could not get serial from sensor %d [%s]"), SEN_ID, SEN_TRAY);
			sensor->serial = "";
			closeHandleWrapper(sensor);
			return 0;
		}
		else {
			sensor->serial = sensor->receive;
			sensor->serial = sensor->serial.substr(4, 5);
			//LOG_F(0, "[%s] Sensor %d [%s] registered", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			closeHandleWrapper(sensor);
			return 1;
		}
	}
	else if (command == "DR") {
		if (!strcmp(sensor->receive, "D R 192\r")) {
			sensor->deviceType = 0;
			LOG_F(0, gettext("[%s] Sensor %d [%s] device type: Security"), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			closeHandleWrapper(sensor);
			return 1;
		}
		if (!strcmp(sensor->receive, "D R 132\r")) {
			sensor->deviceType = 1;
			LOG_F(0, gettext("[%s] Sensor %d [%s] device type: Dual"), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			closeHandleWrapper(sensor);
			return 1;
		}
		if (!strcmp(sensor->receive, "D R 129\r")) {
			sensor->deviceType = 2;
			LOG_F(0, gettext("[%s] Sensor %d [%s] device type: Sounder"), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			closeHandleWrapper(sensor);
			return 1;
		}
		if (!strcmp(sensor->receive, "D R 128\r")) {
			sensor->deviceType = 3;
			LOG_F(0, gettext("[%s] Sensor %d [%s] device type: Beacon"), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			closeHandleWrapper(sensor);
			return 1;
		}
		LOG_F(2, "[%s] Could not get device type from sensor %d [%s]", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
		closeHandleWrapper(sensor);
		return 0;
	}
	else if (command == "MB") {
		for (int i = 0; i < 70; i++)
			if (sensor->receive[i] == ',')
				commaCounter++;
		if (commaCounter != 13) {//first check that mb returned the correct format
			LOG_F(2, gettext("[%s] Sensor %d [%s] Bad MB read from % s"), sensor->serial.c_str(), SEN_ID, SEN_TRAY, sensor->COM.c_str());
			closeHandleWrapper(sensor);
			return 0;
		}
		int k = 0;
		int j;
		int i;
		for (i = 0; i < 14; i++) {
			for (j = 0; j < 4 && sensor->receive[k] != ',' && k < 62; j++) {
				if (sensor->receive[k] == '-') {
					sensor->MBreadArray[i][j] = '0';
					LOG_F(1, gettext("[%s] Negative smoke value detected at sensor %d [%s], opening ports"), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
					sensor->validRead = false;
					openPorts(sensor);
					break;

				}
				sensor->MBreadArray[i][j] = sensor->receive[k];
				k++;
			}

			if (sensor->receive[k] == ',') {
				sensor->MBreadArray[i][j] = '\0';
			}

			if (i == 0)
				sensor->TT1 = std::stoi(sensor->MBreadArray[i]);
			else if (i == 1)
				sensor->TT2 = std::stoi(sensor->MBreadArray[i]);
			else if (i == 2)
				sensor->TT3 = std::stoi(sensor->MBreadArray[i]);
			else if (i == 3)
				sensor->TT4 = std::stoi(sensor->MBreadArray[i]);
			else if (i == 4)
				sensor->TT5 = std::stoi(sensor->MBreadArray[i]);
			else if (i == 5)
				sensor->DARK_REF = std::stoi(sensor->MBreadArray[i]);
			else if (i == 6)
				sensor->DARK_VALUE = std::stoi(sensor->MBreadArray[i]);
			else if (i == 7)
				sensor->LIGHT_VALUE = std::stoi(sensor->MBreadArray[i]);
			else if (i == 8)
				sensor->LIGHT_REF = std::stoi(sensor->MBreadArray[i]);
			else if (i == 9) {
				sensor->SMOKE = std::stoi(sensor->MBreadArray[i]);

			}
			else if (i == 10)
				sensor->SMOKE_STATUS = std::stoi(sensor->MBreadArray[i]);
			else if (i == 13) {
				sensor->BASE = std::stoi(sensor->MBreadArray[i]);
			}
			k++;


		}
		if (sensor->SMOKE < 30) {
			LOG_F(1, gettext("[%s] Sensor %d [%s] has smoke values well below baseline, opening ports"), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			openPorts(sensor);
			sensor->validRead = false;
			sensor->invalidReadCount++;
		}
		else {
			sensor->invalidReadCount = 0;
			sensor->validRead = true;
		}

	}

	else if (command == "PORTG1111")
		sensor->openingPorts = false;

	if (command != "IR")
		sensor->queue.erase(sensor->queue.begin());
	closeHandleWrapper(sensor);
	return 1;

}
int sendReadBytesTunnel() {
	if (tunnelCOM == "\\\\.\\" || tunnelCOM == "")//check
		return 0;
	while (1) {
		if (!run || tunnelPause) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			continue;
		}
		HANDLE hSerial;

		hSerial = CreateFileA(tunnelCOM.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,    // must be opened with exclusive-access
			NULL, // no security attributes
			OPEN_EXISTING, // must use OPEN_EXISTING
			0,    // not overlapped I/O
			NULL  // hTemplate must be NULL for comm devices
		);

		DCB dcbSerialParams = { 0 };
		COMMTIMEOUTS timeouts = { 0 };
		OVERLAPPED ol = { 0 };
		dcbSerialParams.DCBlength = sizeof(dcbSerialParams);

		if (!GetCommState(hSerial, &dcbSerialParams)) {
			LOG_F(2, gettext("Error opening tunnel COM with %s in use or does not exist"), tunnelCOM.c_str());

			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			for (int a = 0; a < 15; a++)
				tunnelReadings[a] = "N/A";
			tunnelConnected = false;
			CloseHandle(hSerial);
			continue;
		}

		dcbSerialParams.BaudRate = CBR_9600;
		dcbSerialParams.ByteSize = 8;
		dcbSerialParams.StopBits = ONESTOPBIT;
		dcbSerialParams.Parity = NOPARITY;

		if (!SetCommState(hSerial, &dcbSerialParams))  //Configuring the port according to settings in DCB
		{
			CloseHandle(hSerial);
			LOG_F(2, "Error in SetCommState() at tunnel port");
			return 1;
		}

		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutConstant = 1000;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 50;
		timeouts.WriteTotalTimeoutMultiplier = 10;
		if (SetCommTimeouts(hSerial, &timeouts) == 0)
		{
			LOG_F(2, "Error setting timeouts for tunnel port");
			CloseHandle(hSerial);
			return 1;
		}

		///*----------------------------- Writing a Character to Serial Port----------------------------------------*/
		while (tunnelCommands.size()) {
			std::string command = tunnelCommands.front();
			int length = (int)strlen(command.c_str());

			char* send = (char*)malloc((size_t)length + 1);
			if (send) {
				strcpy_s(send, (size_t)length + 1, command.c_str());
				send[length] = 13;
			}
			DWORD  dNoOFBytestoWrite;              // No of bytes to write into the port
			DWORD  dNoOfBytesWritten = 0;          // No of bytes written to the port
			dNoOFBytestoWrite = length + 1; // Calculating the no of bytes to write into the port

			if (!WriteFile(hSerial, send, dNoOFBytestoWrite, &dNoOfBytesWritten, NULL)) {
				LOG_F(2, "Could not send bytes to tunnel");
				CloseHandle(hSerial);
			}
			tunnelCommands.erase(tunnelCommands.begin());
			if (tunnelCommands.size()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(400));
			}
		}
		int maxChars = 46;
		BOOL  Read_Status;                      // Status of the various operations 
		char receive[47];// Buffer Containing Rxed Data
		for (int i = 0; i < maxChars; i++)
			receive[i] = 0;
		DWORD NoBytesRead;                     // Bytes read by ReadFile()
		///*------------------------------------ Setting Receive Mask ----------------------------------------------*/
		Read_Status = SetCommMask(hSerial, EV_RXCHAR); //Configure Windows to Monitor the serial device for Character Reception
		if (!ReadFile(hSerial, receive, 46, &NoBytesRead, NULL)) {
			tunnelConnected = false;
			LOG_F(2, gettext("Failed at reading tunnel bytes"));
			CloseHandle(hSerial);
			continue;
		}
		if (!NoBytesRead || !receive[1]) {
			for (int a = 0; a < 15; a++)
				tunnelReadings[a] = "N/A";
			tunnelConnected = false;
			LOG_F(2, gettext("No tunnel data received"));
			CloseHandle(hSerial);
			continue;
		}

		if (Read_Status == FALSE) {
			LOG_F(2, "Error in setting tunnel CommMask");
			tunnelConnected = false;
			CloseHandle(hSerial);
			continue;
		}
		char temp[10];
		int k = 0;
		int j = 0;
		int format[15] = { 5,5,5,4,2,1,1,1,1,1,1,1,1,1,1 };
		for (int i = 0; i < maxChars; i++) {
			if (receive[i] == ' ') {
				if (k != format[j]) {
					for (int a = 0; a < 15; a++)
						tunnelReadings[a] = "N/A";
					LOG_F(2, gettext("Failed reading tunnel data"));
					tunnelConnected = false;
					break;
				}
				temp[k] = '\0';
				tunnelReadings[j++] = &temp[0];
				k = 0;
			}
			else
				temp[k++] = receive[i];

		}
		tunnelReadings[j] = &temp[0]; // append last reading
		tunnelConnected = true;

		CloseHandle(hSerial);
	}
	return 1;
}
int COMSetup(bool firstTimeSetup) {
	static int setups = 0;
	setups++;
	static std::mutex mtx;
	inComSetup = true;
	if (!firstTimeSetup)
		std::this_thread::sleep_for(std::chrono::seconds(5));
	mtx.lock();

	if (setups > 1) {
		setups--;
		mtx.unlock();

		return 0;
	}

	inComSetup = true;
	serialHeaderChanged = true;
	std::ifstream file("config/COMs.txt");
	std::string line;
	char* portName[256];
	for (int i = 0; i < 256; i++)
		portName[i] = (char*)calloc(256, 1);
	int n = 0;
	if (!firstTimeSetup) {
		run = false;
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		tick = 0;
		for (int i = 0; i < SENSORS; i++) {
			sen[i].completedTest = -1;
			sen[i].completedSegment = 0;
			sen[i].invalidReadCount = 0;
			sen[i].COM = "";
			sen[i].serial = "";
			sen[i].activated = false;
			sen[i].disconnectedCounter = 0;
			sen[i].fCTS = 0;
			sen[i].deviceType = 4;
			sen[i].sdata.Erase();
			sen[i].sdata2.Erase();
			sen[i].faultyCOM = true;
			sen[i].Assembled_Ident_Number = "";
			sen[i].Type_ID = "0";
			sen[i].Decommissioned = false;
			sen[i].Works_Order_Number = "0";
			sen[i].Revision = "0";
			sen[i].Device_Type = "";
			sen[i].Brand_ID = "";
			sen[i].Equivalent_Type_ID = "";
			sen[i].EFACS_Part_Number = "";
			sen[i].Mainboard_ID = "";
			sen[i].Tests_Passed = 0;
			sen[i].validRead = false;
			sen[i].mrReceive = "";
			sen[i].TT1 = 0;
			sen[i].TT2 = 0;
			sen[i].TT3 = 0;
			sen[i].TT4 = 0;
			sen[i].TT5 = 0;
			sen[i].SMOKE = 0;
			sen[i].SMOKE_STATUS = 0;
			sen[i].BASE = 0;
			sen[i].LIGHT_REF = 0;
			sen[i].LIGHT_VALUE = 0;
			sen[i].DARK_REF = 0;
			sen[i].DARK_VALUE = 0;
			//sen[i].hSerial = NULL;
			sen[i].serial = "";
			//sen[i].queue.clear();
			sen[i].isClosed = true;
			sen[i].testPassed = "N/A";
			sen[i].openingPorts = false;
			sen[i].sendRTS = true;

		}
	}
	LOG_F(0, gettext("Scanning for COMs"));
	EnumerateComPortQueryDosDevice(&n, portName);
	LOG_F(0, gettext("Found %d COMs"), n);
	char buffer[100];
	bool tunnelCOMFound = false;
	if (file) { //check check its valid file. assuming it is
		std::vector<std::string> COMs;
		while (std::getline(file, line)) {
			if (line.find("\tsensor ") != -1) {
				int m = 0;
				COMs.push_back(line.substr(m = (line.find(":")) + 1));
			}
			else if (line.find("TunnelCOM:COM") != -1) {
				tunnelCOM = "\\\\.\\" + line.substr(line.find(":") + 1);
				tunnelCOMFound = true;
			}
		}
		int match = 0;
		for (int i = 0; i < n; i++) {
			for (int j = 0; j < 4; j++)
				if (!strcmp(portName[i], COMs[j * 16].c_str())) {
					if (++match == 2) {
						tray_2_ID = j;
						break;
					}
					else
						tray_1_ID = j;
				}
			if (match == 2)
				break;
		}
		int coms = 0;
		for (int i = 0; i < 32; i++)
			if (COM_selectable[i] == NULL)
				coms++;
		if (match)
			for (int i = 0; i < SENSORS / 2; i++) {
				snprintf(buffer, 100, "\\\\.\\%s", COMs[i + tray_1_ID * 16].c_str());
				sen[i].COM = buffer;
			}
		if (match == 2)
			for (int i = 0; i < SENSORS / 2; i++) {
				snprintf(buffer, 100, "\\\\.\\%s", COMs[i + tray_2_ID * 16].c_str());
				sen[i + 16].COM = buffer;
			}
		else if (!match && coms == 32) {
			LOG_F(1, gettext("Could not find any corresponding tray setup with current COM devices"));
		}
		file.close();
	}
	else {
		LOG_F(1, gettext("Could not find or open tray setup file (COMs.TXT), generating new file"));
		generateTraySetup();
		COMRescan();
	}

	if (!tunnelCOMFound)
		LOG_F(2, gettext("No tunnel COM in COMs.txt specified"));

	for (int j = 0; j < SENSORS; j++)
		COM_selectable[j] = sen[j].COM.c_str();

	run = true;
	if (firstTimeSetup) {
		std::thread tunnel(sendReadBytesTunnel);
		tunnel.detach();
	}
	inComSetup = false;
	setups = 0;
	mtx.unlock();
	return 0;
}
int setAlarmThreshold(sensorStruct* sensor, int* status, calibrationPresets preset, int noSensors, bool skipSql) {
	sensor->completedTest = 0;
	int sqlStatus = 0;
	*status = 1;
	LOG_F(0, gettext("[%s] Setting alarm thresholds for sensor %d [%s] "), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	Idle_Time_End = std::chrono::steady_clock::now();
	static std::mutex mtx;
	static bool sentStopCommand;
	sentStopCommand = false;
	bool testMode = false;
	int timeout = 3;
	int DEVICE_SET_POINT_COUNT_PRE_ALARM = 0;
	int DEVICE_SET_POINT_COUNT_ALARM = 0;
	int AVERAGE_DEVICE_COUNT = 0;
	int DEVICE_BASELINE_COUNT = 0;
	float AVERAGE_TUNNEL_DB = 0.0f;
	float DESIRED_SET_POINT_DB_PRE_ALARM = (sensor->deviceType == 0) ? preset.UTC_preAlarmSetPoint : preset.preAlarmSetPoint;
	float DESIRED_SET_POINT_DB_ALARM = (sensor->deviceType == 0) ? preset.UTC_alarmSetPoint : preset.alarmSetPoint;
	timeout = 720; //12mins @ 1 refresh rate
	float rangeMin = preset.min;
	float rangeMax = preset.max;
	int samples = preset.samples;
	int readFromScatter = preset.readFromScatter;
	int consecutiveRead = 0;
	//----sql-----
	bool sqlFailed = false;
	float ALARM_HIGH_VALUE = 0.0f;
	float ALARM_LOW_VALUE = 0.0f;
	float ALARM_MEDIUM_VALUE = 0.0f;
	std::string Assembled_Ident_Number = "";
	int Baseline_Delta = 0;
	float Beam_Zero_Point = 0.0f;
	float CALIBRATION_AIR_SPEED = 0.0f;
	float CALIBRATION_BASLINE = 0.0f;
	float CALIBRATION_CHAMBER_POSITION = 0.0f;
	float CALIBRATION_HEAT_VALUE_1 = 0.0f;
	float CALIBRATION_HEAT_VALUE_2 = 0.0f;
	float CALIBRATION_HEAT_VALUE_3 = 0.0f;
	float CALIBRATION_HEAT_VALUE_4 = 0.0f;
	float CALIBRATION_HEAT_VALUE_5 = 0.0f;
	float CALIBRATION_OBSCURATION_VALUE = 0.0f;
	float CALIBRATION_RESULT = 0.0f;
	float CALIBRATION_SMOKE_VALUE = 0.0f;
	float CALIBRATION_TARGET_OBSCURTAION_VALUE = 0.0f;
	float CALIBRATION_TEMPERATURE = 0.0f;
	std::string CheckSum = "";
	float DARK_REF = 0.0f;
	float DARK_VALUE = 0.0f;
	std::string DateTime_of_Test = "";
	float Idle_Time = 0.0f;
	float LIGHT_REF = 0.0f;
	float LIGHT_VALUE = 0.0f;
	int Mainboard_ID = 0;
	float MEDIUM_ALARM_THRESHOLD = 0.0f;
	int Pass_Parameter_Set_Number = 0;
	float PRE_ALARM_HIGH_VALUE = 0.0f;
	float PRE_ALARM_LOW_VALUE = 0.0f;
	float PRE_ALARM_MEDIUM_VALUE = 0.0f;
	int Record_Number = 0;
	std::string Revision = "0";
	std::string Test_Jig_Software_Name = "";
	std::string Test_Jig_Software_Version = "";
	float Test_Time = 0.0f;
	int Tests_Passed = 0;
	std::string Type_ID = "0";
	int User_ID = 0;
	std::string Works_Order_Number = "0";
	char Workstation_Name[150];
	GetMachineName(Workstation_Name);
	char* database;
	bool faultySensor = false;
	if (sensor->deviceType) {
		sensor->Assembled_Ident_Number = "S0" + sensor->serial;
		database = "tracking_smartcell";
	}
	else {
		sensor->Assembled_Ident_Number = "U0" + sensor->serial;
		database = "tracking_utc";
	}

	char buffer[2048];
	std::vector<std::string> results;
	float LowPreScale = 0, MedPreScale = 0, HighPreScale = 1, LowAlarmScale = 1, MedAlarmScale = 1, HighAlarmScale = 1;
	int Baseline_Delta_Lower_Limit = 0;
	int Baseline_Delta_Upper_Limit = 0;
	if (!testMode) {
		snprintf(buffer, sizeof(buffer), "SELECT Assembled_Devices.Type_ID,\
										Assembled_Devices.Assembled_Ident_Number,\
										Assembled_Devices.Decommissioned,\
										Assembled_Devices.Record_Number,\
										Assembled_Devices.Works_Order_Number,\
										Assembled_Devices.Revision,\
										PCB_Software.Current_Software_ID,\
										PCB_Software.Processor_Number,\
										Software.Description,\
										Software.Options_File_Path,\
										Software.Version,\
										Software.File_Path_Name \
			FROM %s.Assembled_Devices LEFT OUTER JOIN %s.PCB_Software ON \
			PCB_Software.Type_ID = Assembled_Devices.Type_ID AND \
			PCB_Software.Revision = Assembled_Devices.Revision \
			LEFT OUTER JOIN %s.Software on Software.Software_ID = PCB_Software.Current_Software_ID \
			WHERE Assembled_Devices.DateTime_Assembled > '2021-02-24 11:03:29'AND \
			Assembled_Devices.Assembled_Ident_Number = '%s' ORDER BY \
			Assembled_Devices.Record_Number DESC LIMIT 1;", database, database, database, sensor->Assembled_Ident_Number.c_str());
		results = ExecuteSql(buffer, &sqlStatus);
		if (sqlStatus == -1)
			sqlFailed = true;
		if (!results.size()) {
			snprintf(buffer, sizeof(buffer), "SELECT Assembled_Devices.Type_ID,\
										Assembled_Devices.Assembled_Ident_Number,\
										Assembled_Devices.Decommissioned,\
										Assembled_Devices.Record_Number,\
										Assembled_Devices.Works_Order_Number,\
										Assembled_Devices.Revision,\
										PCB_Software.Current_Software_ID,\
										PCB_Software.Processor_Number,\
										Software.Description,\
										Software.Options_File_Path,\
										Software.Version,\
										Software.File_Path_Name \
			FROM %s.Assembled_Devices LEFT OUTER JOIN %s.PCB_Software ON \
			PCB_Software.Type_ID = Assembled_Devices.Type_ID AND \
			PCB_Software.Revision = Assembled_Devices.Revision \
			LEFT OUTER JOIN %s.Software on Software.Software_ID = PCB_Software.Current_Software_ID \
			WHERE Assembled_Devices.Assembled_Ident_Number = '%s' ORDER BY \
			Assembled_Devices.Record_Number DESC LIMIT 1;", database, database, database, sensor->Assembled_Ident_Number.c_str());
			results = ExecuteSql(buffer, &sqlStatus);

		}
		if (sqlStatus == -1) //log message will printed from ExecuteSql seperate from the one below
			sqlFailed = true;
		if (results.size() < 5) { //first make sure it's not empty but also that largest index can be referenced
			LOG_F(2, "[%s] Sensor %d [%s] Could not find sensor in SQL database", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			sqlFailed = true;
		}
		else {
			sensor->Type_ID = results[0];
			sensor->Decommissioned = stoi(results[2]);
			sensor->Works_Order_Number = results[4];
			sensor->Revision = results[5];
		}
	}
	if (!sqlFailed) {
		snprintf(buffer, sizeof(buffer), "SELECT Device_Type, Brand_ID, Equivalent_Type_ID FROM %s.Device_Type WHERE Type_ID = '%s'", database, sensor->Type_ID.c_str());
		results = ExecuteSql(buffer, &sqlStatus);
		if (sqlStatus == -1)
			sqlFailed = true;
		if (results.size() > 2) {
			sensor->Device_Type = results[0];
			sensor->Brand_ID = results[1];
			sensor->Equivalent_Type_ID = results[2];
		}
		else {
			LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Device_Type query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			sqlFailed = true;
		}

		snprintf(buffer, sizeof(buffer), "SELECT EFACS_Part_Number FROM %s.EFACS_Part_Number WHERE Type_ID = '%s'", database, sensor->Type_ID.c_str());
		results = ExecuteSql(buffer, &sqlStatus);
		if (sqlStatus == -1)
			sqlFailed = true;
		if (results.size())
			sensor->EFACS_Part_Number = results[0];
		else {
			LOG_F(2, "[%s] Sensor %d [%s] Could not fetch EFACS_Part_Number query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			sqlFailed = true;
		}

		snprintf(buffer, sizeof(buffer), "SELECT Mainboard_ID, Tests_Passed FROM %s.Radiated_Test_Stage\
			WHERE Assembled_Ident_Number = '%s' ORDER BY datetime_of_test DESC LIMIT 1", database, sensor->Assembled_Ident_Number.c_str());
		results = ExecuteSql(buffer, &sqlStatus);
		if (sqlStatus == -1)
			sqlFailed = true;
		if (results.size() > 1) {
			sensor->Mainboard_ID = results[0];
			sensor->Tests_Passed = stoi(results[1]);
		}
		else {
			LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Radiated_Test_Stage query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			sqlFailed = true;
		}

		if (!sensor->Tests_Passed) {
			LOG_F(2, "[%s] Sensor %d [%s] Previous test not passed, could not calibrate", sensor->serial.c_str(), SEN_ID, SEN_TRAY); //check notify
			sensor->testPassed = "PREVIOUS TEST NOT PASSED";
			sensor->completedTest = 1;
			return 1;
		}


		snprintf(buffer, sizeof(buffer), "SELECT Test_Stage_Pass_Parameters.Field_Name,\
			Test_Stage_Pass_Parameters.Lower_Pass_Value,\
			Test_Stage_Pass_Parameters.Upper_Pass_Value,\
			Test_Stage_Pass_Parameters.Type_ID,\
			Test_Stage_Pass_Parameters.Revision,\
			Test_Stage_Pass_Parameters.Pass_Parameter_Set_Number,\
			Test_Stage_Pass_Parameters.Test_Name \
			FROM %s.Test_Stages LEFT OUTER JOIN %s.Test_Stage_Pass_Parameters ON \
			Test_Stage_Pass_Parameters.Type_ID = Test_Stages.Type_ID WHERE Test_Stage_Pass_Parameters.Test_Name = 'Calibration_Test_Stage' AND \
			Test_Stage_Pass_Parameters.Type_ID = '%s' AND Test_Stage_Pass_Parameters.Revision = '%s';", database, database, sensor->Type_ID.c_str(), sensor->Revision.c_str());
		results = ExecuteSql(buffer, &sqlStatus);
		if (sqlStatus == -1) {
			LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Test_Stage_Pass_Parameters query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			sqlFailed = true;
		}

		if (results.size() > 6) {
			for (int i = 0; i < results.size(); i += 7) {
				if (results[i] == "LowPreScale")
					LowPreScale = std::stof(results[i + 2]);
				else if (results[i] == "MedPreScale")
					MedPreScale = std::stof(results[i + 2]);
				else if (results[i] == "HighPreScale")
					HighPreScale = std::stof(results[i + 2]);
				else if (results[i] == "LowAlarmScale")
					LowAlarmScale = std::stof(results[i + 2]);
				else if (results[i] == "MedAlarmScale")
					MedAlarmScale = std::stof(results[i + 2]);
				else if (results[i] == "HighAlarmScale")
					HighAlarmScale = std::stof(results[i + 2]);
				else if (results[i] == "Baseline_Delta") {
					Baseline_Delta_Lower_Limit = std::stoi(results[i + 1]);
					Baseline_Delta_Upper_Limit = std::stoi(results[i + 2]);
				}
				Pass_Parameter_Set_Number = stoi(results[5]);
			}
		}
		else {
			if (skipSql)
				LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Test_Stage_Pass_Parameters query, setting alarm scales to 1", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			else
				LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Test_Stage_Pass_Parameters query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			Baseline_Delta_Upper_Limit = 500;
			Baseline_Delta_Lower_Limit = 120;
			LowPreScale, MedPreScale, HighPreScale, LowAlarmScale, MedAlarmScale, HighAlarmScale = 1;
			sqlFailed = true;
		}

	}
	if (sqlFailed && !skipSql) {
		sensor->testPassed = "MISSING SQL QUERIES";
		sensor->completedTest = 1;
		return 1;
	}
	if (sensor->deviceType) {
		if (MedAlarmScale != 1)
			LOG_F(1, "[%s] Sensor %d [%s] MedAlarmScale not 1, set to %.2f", sensor->serial.c_str(), SEN_ID, SEN_TRAY, MedAlarmScale);
		if (HighAlarmScale != 1)
			LOG_F(1, "[%s] Sensor %d [%s] HighAlarmScale not 1, set to %.2f", sensor->serial.c_str(), SEN_ID, SEN_TRAY, HighAlarmScale);
		if (LowAlarmScale != 1)
			LOG_F(1, "[%s] Sensor %d [%s] LowAlarmScale not 1, set to %.2f", sensor->serial.c_str(), SEN_ID, SEN_TRAY, LowAlarmScale);
	}
	//-------------1681 1684 1680 1679 1682 1720 1732 1811 1734 1733 1736
	sensor->completedSegment = 1;
	timeout = 70;
	static bool waiting = true;
	while (timeout && waiting) {
		int completed = 0;
		for (int i = 0; i < SENSORS; i++)
			if (sen[i].completedSegment)
				completed++;
		if (completed == noSensors) {
			waiting = false;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		timeout--;
	}
	if (!timeout)
		LOG_F(1,"break1");
	sensor->completedSegment = 0;
	if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
		return 0;
	else
		*status = 15;
	if (!testMode) {
		if (sensor->deviceType) {
			openPorts(sensor);
			std::this_thread::sleep_for(std::chrono::seconds(8));//check
		}
	}
	sensor->completedSegment = 1;
	timeout = 70;
	static bool waiting2 = true;
	while (timeout && waiting2) {
		int completed = 0;
		for (int i = 0; i < SENSORS; i++)
			if (sen[i].completedSegment)
				completed++;
		if (completed == noSensors) {
			waiting2 = false;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		timeout--;
	}
	if (!timeout)
		LOG_F(1, "break2");
	sensor->completedSegment = 0;
	if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
		return 0;
	else
		*status = 1;

	int baseline = 0;
	int baselineSamples = 0;
	timeout = 120;
	if (sensor->BASE == 0) {
		LOG_F(0, "[%s] Sensor %d [%s] not baslined, setting baseline", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
		while (timeout > 0) {
			if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
				return 0;
			if (stof(tunnelReadings[1]) < 0.02) {


				if (isFloatNumber(tunnelReadings[1]))
					if (stof(tunnelReadings[1]) < 0.02) {
						baseline += sensor->SMOKE;
						baselineSamples++;
						if (baselineSamples == 5)
							break;

					}
					else {
						baseline = 0;
						baselineSamples = 0;
					}
				float milliseconds = 1000 / REFRESH_RATE;
				std::this_thread::sleep_for(std::chrono::milliseconds((int)milliseconds));
				timeout--;

			}
			else {
				mtx.lock();
				if (!sentStopCommand) {
					tunnelCommands.push_back("STP");
					sentStopCommand = true;
				}
				mtx.unlock();
			}
		}
		if (!timeout) {
			LOG_F(2, "Timeout reached trying to baseline sensor");
			if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
				return 1;
		}
		baseline /= baselineSamples;
		char buff[100];
		snprintf(buff, 100, "MN %d", baseline);
		sensor->queue.push_back(buff);

	}
	if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
		return 0;
	sensor->completedSegment = 1;
	timeout = 70;
	static bool waiting3 = true;
	while (timeout && waiting3) {
		if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
			return 0;
		int completed = 0;
		for (int i = 0; i < SENSORS; i++)
			if (sen[i].completedSegment)
				completed++;
		if (completed == noSensors) {
			waiting3 = false;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		timeout--;
	}
	sensor->completedSegment = 0;
	int minSmoke = 0;
	int maxSmoke = 0;
	timeout = 720;
	while (timeout > 0) {
		if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
			return 0;

		if (isFloatNumber(tunnelReadings[1])) {
			if (stof(tunnelReadings[readFromScatter]) >= rangeMin && stof(tunnelReadings[readFromScatter]) <= rangeMax && sensor->validRead) {
				if (sensor->SMOKE > maxSmoke)
					maxSmoke = sensor->SMOKE;
				if (minSmoke == 0)
					minSmoke = sensor->SMOKE;
				if (sensor->SMOKE < minSmoke)
					minSmoke = sensor->SMOKE;
				consecutiveRead++;
				*status = 2;
				AVERAGE_DEVICE_COUNT += sensor->SMOKE;
				CALIBRATION_AIR_SPEED += stof(tunnelReadings[3]);
				CALIBRATION_SMOKE_VALUE += sensor->SMOKE;
				CALIBRATION_TEMPERATURE += stof(tunnelReadings[2]);
				CALIBRATION_OBSCURATION_VALUE += stof(tunnelReadings[1]);
				CALIBRATION_HEAT_VALUE_1 += sensor->TT1;
				CALIBRATION_HEAT_VALUE_2 += sensor->TT2;
				CALIBRATION_HEAT_VALUE_3 += sensor->TT3;
				CALIBRATION_HEAT_VALUE_4 += sensor->TT4;
				CALIBRATION_HEAT_VALUE_5 += sensor->TT5;
				AVERAGE_TUNNEL_DB += stof(tunnelReadings[readFromScatter]);
				DARK_REF += sensor->DARK_REF;
				DARK_VALUE += sensor->DARK_VALUE;
				LIGHT_REF += sensor->LIGHT_REF;
				LIGHT_VALUE += sensor->LIGHT_VALUE;
				if (consecutiveRead == samples)
					break;
			}
			else {
				CALIBRATION_HEAT_VALUE_1 = 0;
				CALIBRATION_HEAT_VALUE_2 = 0;
				CALIBRATION_HEAT_VALUE_3 = 0;
				CALIBRATION_HEAT_VALUE_4 = 0;
				CALIBRATION_HEAT_VALUE_5 = 0;
				CALIBRATION_SMOKE_VALUE = 0;
				consecutiveRead = 0;
				CALIBRATION_AIR_SPEED = 0;
				CALIBRATION_TEMPERATURE = 0;
				CALIBRATION_OBSCURATION_VALUE = 0;
				AVERAGE_TUNNEL_DB = 0;
				AVERAGE_DEVICE_COUNT = 0;
				DARK_REF = 0;
				DARK_VALUE = 0;
				LIGHT_REF = 0;
				LIGHT_VALUE = 0;
				minSmoke = 0;
				maxSmoke = 0;

			}
			if (sensor->invalidReadCount > 10) {
				faultySensor = true;
				break;
			}
		}

		float milliseconds = 1000 / REFRESH_RATE;
		std::this_thread::sleep_for(std::chrono::milliseconds((int)milliseconds));
		timeout--;//check
	}
	if (faultySensor) {
		LOG_F(2, "Faulty sensor, could not calibrate because it kept trying to open ports");
		sensor->testPassed = "FAILED";
		sensor->completedTest = 1;
		return 1;
	}
	if (!timeout) {
		*status = 12;
		sensor->completedTest = 1;
		return 0;
	}
	sensor->completedSegment = 1;
	timeout = 70;
	static bool waiting4 = true;
	while (timeout && waiting4) {
		if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
			return 0;
		int completed = 0;
		for (int i = 0; i < SENSORS; i++)
			if (sen[i].completedSegment)
				completed++;
		if (completed == noSensors) {
			waiting4 = false;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(400));
		timeout--;
	}
	if (!timeout)
		LOG_F(1, "break4");
	sensor->completedSegment = 0;

	AVERAGE_DEVICE_COUNT /= samples;
	CALIBRATION_AIR_SPEED /= samples;
	CALIBRATION_TEMPERATURE /= samples;
	CALIBRATION_SMOKE_VALUE /= samples;

	AVERAGE_TUNNEL_DB /= samples;
	DARK_REF /= samples;
	DARK_VALUE /= samples;
	LIGHT_REF /= samples;
	LIGHT_VALUE /= samples;
	CALIBRATION_OBSCURATION_VALUE /= samples;
	CALIBRATION_HEAT_VALUE_1 /= samples;
	CALIBRATION_HEAT_VALUE_2 /= samples;
	CALIBRATION_HEAT_VALUE_3 /= samples;
	CALIBRATION_HEAT_VALUE_4 /= samples;
	CALIBRATION_HEAT_VALUE_5 /= samples;

	float offset = 1;//.085;//0.87;
	DEVICE_BASELINE_COUNT = atoi(sensor->MBreadArray[13]);
	DEVICE_SET_POINT_COUNT_PRE_ALARM = (int)(((AVERAGE_DEVICE_COUNT - DEVICE_BASELINE_COUNT) / AVERAGE_TUNNEL_DB) * DESIRED_SET_POINT_DB_PRE_ALARM) + DEVICE_BASELINE_COUNT;
	DEVICE_SET_POINT_COUNT_ALARM = (int)(((AVERAGE_DEVICE_COUNT - DEVICE_BASELINE_COUNT) / AVERAGE_TUNNEL_DB) * DESIRED_SET_POINT_DB_ALARM) + DEVICE_BASELINE_COUNT;


	DEVICE_SET_POINT_COUNT_ALARM *= offset;
	//DEVICE_SET_POINT_COUNT_PRE_ALARM *= offset;
	//results = ExecuteSql("SELECT Baseline_Delta FROM Test_Stage_Pass_Parameters");
	//avg: -0.0844846 offset smartcell

//0.129904 offset utc

//-0.0186212 verify utc

//-0.0109507 verify smartcell
	int MHP = 0, MHA = 0, MMP = 0, MMA = 0, MLP = 0, MLA = 0;
	baseline = sensor->BASE;
	MHP = ((DEVICE_SET_POINT_COUNT_PRE_ALARM - baseline) * HighPreScale) + baseline;
	MMP = ((DEVICE_SET_POINT_COUNT_PRE_ALARM - baseline) * MedPreScale) + baseline;
	MLP = ((DEVICE_SET_POINT_COUNT_PRE_ALARM - baseline) * LowPreScale) + baseline;
	MHA = ((DEVICE_SET_POINT_COUNT_ALARM - baseline) * HighAlarmScale) + baseline;
	MMA = ((DEVICE_SET_POINT_COUNT_ALARM - baseline) * MedAlarmScale) + baseline;
	MLA = ((DEVICE_SET_POINT_COUNT_ALARM - baseline) * LowAlarmScale) + baseline;
	if (DEVICE_SET_POINT_COUNT_PRE_ALARM == 0 || DEVICE_SET_POINT_COUNT_ALARM == 0) {
		LOG_F(2, gettext("=========FAILED SETTING THRESHOLDS=========="));
	}
	if (DEVICE_SET_POINT_COUNT_ALARM - sensor->BASE < Baseline_Delta_Lower_Limit
		|| DEVICE_SET_POINT_COUNT_ALARM - sensor->BASE > Baseline_Delta_Upper_Limit) {
		sensor->testPassed = "FAILED";
	}
	else {
		LOG_F(0, "[%s] Sensor %d [%s]: Attempting to set MHA: %d MMA: %d MLA: %d MHP: %d MMP: %d MLP: %d",
			sensor->serial.c_str(), SEN_ID, SEN_TRAY, MHA, MMA, MLA, MHP, MMP, MLP);
		LOG_F(0, gettext("[%s] Sensor %d [%s]: AVERAGE_DEVICE_COUNT: %d DEVICE_BASELINE_COUNT: %d AVERAGE_TUNNEL_DB: %.3f MAX READ: %d MIN READ: %d"),// group: %d db:%.3f"),
			sensor->serial.c_str(), SEN_ID, SEN_TRAY, AVERAGE_DEVICE_COUNT, DEVICE_BASELINE_COUNT, AVERAGE_TUNNEL_DB, maxSmoke,minSmoke); //, group, DESIRED_SET_POINT_DB_ALARM);
	}
	if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
		return 0;


	*status = 3;



	snprintf(buffer, 99, "MHP %s", std::to_string(MHP).c_str());
	sensor->queue.push_back(buffer);
	snprintf(buffer, 99, "MMP %s", std::to_string(MMP).c_str());
	sensor->queue.push_back(buffer);
	snprintf(buffer, 99, "MLP %s", std::to_string(MLP).c_str());
	sensor->queue.push_back(buffer);

	snprintf(buffer, 99, "MHA %s", std::to_string(MHA).c_str());
	sensor->queue.push_back(buffer);
	snprintf(buffer, 99, "MMA %s", std::to_string(MMA).c_str());
	sensor->queue.push_back(buffer);
	snprintf(buffer, 99, "MLA %s", std::to_string(MLA).c_str());
	sensor->queue.push_back(buffer);
	sensor->queue.push_back("MR");//verify

	timeout = 20;

	int originalBaseline = 0;
	int alarmSet;

	while (timeout) {
		if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
			return 0;
		std::string mr = sensor->mrReceive;
		std::vector<std::string> mrVec;
		if (mr.find("Normal Baseline") != -1) {
			mr.erase(std::remove(mr.begin(), mr.end(), '\n'), mr.end());
			tokenize(mr, '\r', mrVec);
			if (isNumber(mrVec[7].substr(13, 3)))
				originalBaseline = stoi(mrVec[7].substr(13, 3));
			if (!isNumber(mrVec[0].substr(4, 3)) || !isNumber(mrVec[2].substr(4, 3)) || !isNumber(mrVec[4].substr(4, 3))
				|| !isNumber(mrVec[1].substr(4, 3)) || !isNumber(mrVec[5].substr(4, 3))) {
				LOG_F(2, gettext("[%s] Sensor %d [%s]: Failed converting string values from MR to int"),
					sensor->serial.c_str(), SEN_ID, SEN_TRAY);
				sensor->testPassed = gettext("FAILED");
				sensor->completedTest = 1;
				return 1;
			}
			alarmSet = stoi(mrVec[1].substr(4, 3));
			if (stoi(mrVec[0].substr(4, 3)) == MHP && stoi(mrVec[2].substr(4, 3)) == MMP
				&& std::stoi(mrVec[4].substr(4, 3)) == MLP && DEVICE_SET_POINT_COUNT_PRE_ALARM > 100 && DEVICE_SET_POINT_COUNT_PRE_ALARM < 1500) {
				if (stoi(mrVec[1].substr(4, 3)) == MHA && stoi(mrVec[3].substr(4, 3)) == MMA
					&& std::stoi(mrVec[5].substr(4, 3)) == MLA
					&& DEVICE_SET_POINT_COUNT_ALARM - sensor->BASE >= Baseline_Delta_Lower_Limit
					&& DEVICE_SET_POINT_COUNT_ALARM - sensor->BASE <= Baseline_Delta_Upper_Limit) {
					sensor->testPassed = gettext("PASSED");
					CALIBRATION_RESULT = 1;
					Tests_Passed = 1;
				}
				else {
					LOG_F(2, gettext("[%s] Sensor %d [%s]: Failed setting alarm threshold, attempted to set MHA: %d MMA: %d MLA: %d but got MHA: %d MMA: %d MLA: %d. Baseline delta at: %d"),
						sensor->serial.c_str(), SEN_ID, SEN_TRAY, MHA, MMA, MLA, stoi(mrVec[1].substr(4, 3)), stoi(mrVec[3].substr(4, 3)), stoi(mrVec[5].substr(4, 3)), DEVICE_SET_POINT_COUNT_ALARM - sensor->BASE);
				}
				break;
			}
			else {
				LOG_F(2, gettext("[%s] Sensor %d [%s]: Failed setting pre alarm threshold, attempted to set MHP: %d MMP: %d MLP: %d but got MHP: %d MMP: %d MLP: %d"),
					sensor->serial.c_str(), SEN_ID, SEN_TRAY, MHP, MMP, MLP, stoi(mrVec[0].substr(4, 3)), stoi(mrVec[2].substr(4, 3)), stoi(mrVec[4].substr(4, 3)));
			}
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		timeout--;
	}
	if (!timeout) {
		sensor->testPassed = gettext("FAILED");
		LOG_F(2, gettext("Timeout reached trying to verify calibration values"));
		return 1;
	}

	if (Tests_Passed != 1)
		sensor->testPassed = gettext("FAILED"); //check notify

	sensor->mrReceive = "";
	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

	Test_Time = std::chrono::duration_cast<std::chrono::seconds>(end - begin).count();
	std::this_thread::sleep_for(std::chrono::seconds(3));
	int cSum = 0;
	if (sensor->deviceType == 0) {
		int ident = std::stoul(sensor->serial, nullptr, 16);
		int freq = 0;
		if (sensor->Equivalent_Type_ID == "11")
			freq = 1280;//0x500
		if (sensor->Equivalent_Type_ID == "22")
			freq = 2560;//0xA00
		cSum = ident + originalBaseline + freq + MHP + MMP + MLP + MHA + MMA + MLA;
		CheckSum = int_to_hex(cSum);
	}


	if (sensor->deviceType == 0) {
		sensor->queue.push_back("ECW" + CheckSum);


	}

	if (!sqlFailed && !testMode) {
		ALARM_HIGH_VALUE = MHA;
		ALARM_LOW_VALUE = MLA;
		ALARM_MEDIUM_VALUE = MMA;
		Assembled_Ident_Number = sensor->serial;
		Baseline_Delta = ALARM_MEDIUM_VALUE - sensor->BASE;
		Beam_Zero_Point = 0.0f;
		//CALIBRATION_AIR_SPEED;
		CALIBRATION_BASLINE = sensor->BASE;
		CALIBRATION_CHAMBER_POSITION = sensor->id + 1;
		//CALIBRATION_HEAT_VALUE_1;
		//CALIBRATION_HEAT_VALUE_2;
		//CALIBRATION_HEAT_VALUE_3;
		//CALIBRATION_HEAT_VALUE_4;
		//CALIBRATION_HEAT_VALUE_5;
		//CALIBRATION_OBSCURATION_VALUE; 
		//CALIBRATION_RESULT;
		CALIBRATION_SMOKE_VALUE;
		CALIBRATION_TARGET_OBSCURTAION_VALUE = 0.2f;
		//CALIBRATION_TEMPERATURE;
		CheckSum = int_to_hex(cSum);
		//DARK_REF;
		//DARK_VALUE;
		DateTime_of_Test = getCurrentDateTime("now");
		Idle_Time = std::chrono::duration_cast<std::chrono::seconds>(Idle_Time_End - Idle_Time_Begin).count();
		//LIGHT_REF;
		//LIGHT_VALUE;
		MEDIUM_ALARM_THRESHOLD = CALIBRATION_SMOKE_VALUE;
		//Pass_Parameter_Set_Number; 
		PRE_ALARM_HIGH_VALUE = MHP;
		PRE_ALARM_LOW_VALUE = MLP;
		PRE_ALARM_MEDIUM_VALUE = MMP;
		Mainboard_ID = isNumber(sensor->Mainboard_ID) ? std::stoi(sensor->Mainboard_ID) : 0;
		//Record_Number;
		Revision = sensor->Revision;
		Test_Jig_Software_Name = SOFTWARE_NAME;
		Test_Jig_Software_Version = VERSION;
		//Test_Time;
		//Tests_Passed;
		Type_ID = sensor->Type_ID;
		User_ID = isNumber(user.serial) ? stoi(user.serial) : 0;
		Works_Order_Number = sensor->Works_Order_Number;
		GetMachineName(Workstation_Name);

		if (skipSql)
			Tests_Passed = 0;

		char buff[2000];
		snprintf(buff, 2000, "INSERT INTO %s.CALIBRATION_TEST_STAGE(ALARM_HIGH_VALUE, ALARM_LOW_VALUE, ALARM_MEDIUM_VALUE, Assembled_Ident_Number, Baseline_Delta, Beam_Zero_Point, CALIBRATION_AIR_SPEED, \
			CALIBRATION_BASELINE, CALIBRATION_CHAMBER_POSITION, CALIBRATION_HEAT_VALUE_1, CALIBRATION_HEAT_VALUE_2, CALIBRATION_HEAT_VALUE_3, CALIBRATION_HEAT_VALUE_4, CALIBRATION_HEAT_VALUE_5, \
			CALIBRATION_OBSCURATION_VALUE, CALIBRATION_RESULT, CALIBRATION_SMOKE_VALUE, CALIBRATION_TARGET_OBSCURATION_VALUE, CALIBRATION_TEMPERATURE, CheckSum, DARK_REF, DARK_VALUE, \
			DateTime_of_Test, Idle_Time, LIGHT_REF, LIGHT_VALUE, Mainboard_ID, MEDIUM_ALARM_THRESHOLD, Pass_Parameter_Set_Number, PRE_ALARM_HIGH_VALUE, PRE_ALARM_LOW_VALUE, PRE_ALARM_MEDIUM_VALUE, \
			Revision, Test_Jig_Software_Name, Test_Jig_Software_Version, Test_Time, Tests_Passed, Type_ID, User_ID, Works_Order_Number, Workstation_Name)\
		Values (%.5f, %.5f, %.5f, '%s',%d, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, %.5f, '%s', %.5f, %.5f, '%s', %.5f, %.5f, %.5f, %d, \
	%.5f, '%d', %.5f, %.5f, %.5f, '%s', '%s', '%s', %.5f, %d, '%s', %d, '%s', '%s');", database, ALARM_HIGH_VALUE, ALARM_LOW_VALUE, ALARM_MEDIUM_VALUE, sensor->Assembled_Ident_Number.c_str(), Baseline_Delta, Beam_Zero_Point, \
			CALIBRATION_AIR_SPEED, CALIBRATION_BASLINE, CALIBRATION_CHAMBER_POSITION, CALIBRATION_HEAT_VALUE_1, CALIBRATION_HEAT_VALUE_2, CALIBRATION_HEAT_VALUE_3, CALIBRATION_HEAT_VALUE_4, \
			CALIBRATION_HEAT_VALUE_5, CALIBRATION_OBSCURATION_VALUE, CALIBRATION_RESULT, CALIBRATION_SMOKE_VALUE, CALIBRATION_TARGET_OBSCURTAION_VALUE, CALIBRATION_TEMPERATURE, CheckSum.c_str(), \
			DARK_REF, DARK_VALUE, DateTime_of_Test.c_str(), Idle_Time, LIGHT_REF, LIGHT_VALUE, Mainboard_ID, MEDIUM_ALARM_THRESHOLD, Pass_Parameter_Set_Number, PRE_ALARM_HIGH_VALUE, PRE_ALARM_LOW_VALUE, \
			PRE_ALARM_MEDIUM_VALUE, Revision.c_str(), Test_Jig_Software_Name.c_str(), Test_Jig_Software_Version.c_str(), Test_Time, Tests_Passed, Type_ID.c_str(), User_ID, Works_Order_Number.c_str(), Workstation_Name);
		ExecuteSql(buff, &sqlStatus);
	}
	if (sqlStatus == -1) {
		sensor->testPassed = "SQL INSERT FAILED";
		sensor->completedTest = 1;
		return 1;
	}
	sensor->completedTest = 1;
	timeout = 60;

	while (timeout) {
		int completed = 0;
		for (int i = 0; i < SENSORS; i++) {
			if (sen[i].completedTest == 1)
				completed++;
		}
		if (completed == noSensors) {
			if (*status != 4) {
				*status = 5;
				Idle_Time_Begin = std::chrono::steady_clock::now();

			}
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		timeout--;
	}
	if (!timeout)
		LOG_F(1, "break5");
	//timeout

	return 1;
}
BOOL EnumerateComPortQueryDosDevice(int* pNumber, char** pPortName) {
	int i, jj = 0;
	int ret;

	OSVERSIONINFOEX osvi;
	ULONGLONG dwlConditionMask;
	DWORD dwChars;

	char* pDevices;
	int nChars;

	ret = FALSE;

	memset(&osvi, 0, sizeof(osvi));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	osvi.dwPlatformId = VER_PLATFORM_WIN32_NT;
	dwlConditionMask = 0;

	VER_SET_CONDITION(dwlConditionMask, VER_PLATFORMID, VER_EQUAL);

	if (FALSE == VerifyVersionInfo(&osvi, VER_PLATFORMID, dwlConditionMask))
	{
		DWORD dwError = GetLastError();
		LOG_F(2, "VerifyVersionInfo error, %d\n", dwError);
		return -1;
	}/*if*/



	nChars = 4096;
	pDevices = (char*)HeapAlloc(GetProcessHeap(),
		HEAP_GENERATE_EXCEPTIONS, nChars * sizeof(char));

	while (0 < nChars)
	{
		dwChars = QueryDosDevice(NULL, pDevices, nChars);

		if (0 == dwChars)
		{
			DWORD dwError = GetLastError();

			if (ERROR_INSUFFICIENT_BUFFER == dwError)
			{
				nChars *= 2;
				HeapFree(GetProcessHeap(), 0, pDevices);
				pDevices = (char*)HeapAlloc(GetProcessHeap(),
					HEAP_GENERATE_EXCEPTIONS, nChars * sizeof(char));

				continue;
			}/*if ERROR_INSUFFICIENT_BUFFER == dwError*/

			LOG_F(2, "QueryDosDevice error, %d\n", dwError);
			return -1;
		}/*if */


		//printf("dwChars = %d\n", dwChars);
		i = 0;
		jj = 0;
		while (pDevices[i])
		{
			char* pszCurrentDevice;
			size_t nLen;
			pszCurrentDevice = &(pDevices[i]);
			nLen = _tcslen(pszCurrentDevice);

			//_tprintf(TEXT("%s\n"), &pTargetPathStr[i]);
			if (3 < nLen)
			{
				if ((0 == _tcsnicmp(pszCurrentDevice, TEXT("COM"), 3))
					&& FALSE != isdigit(pszCurrentDevice[3]))
				{
					//Work out the port number
					strcpy_s(pPortName[jj++], 256, pszCurrentDevice);


				}
			}

			i += ((int)nLen + 1);
		}

		break;
	}/*while*/

	if (NULL != pDevices)
		HeapFree(GetProcessHeap(), 0, pDevices);


	*pNumber = jj;

	if (0 < jj)
		ret = TRUE;

	return ret;
}
inline std::string getCurrentDateTime(std::string s) {
	time_t now = time(0);
	struct tm  tstruct;
	char  buf[80] = "";
	localtime_s(&tstruct, &now);
	if (s == "now")
		strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
	else if (s == "time")
		strftime(buf, sizeof(buf), "%X", &tstruct);
	else if (s == "date")
		strftime(buf, sizeof(buf), "%Y-%m-%d", &tstruct);
	else if (s == "day")
		strftime(buf, sizeof(buf), "%A", &tstruct);
	else if (s == "hour")
		strftime(buf, sizeof(buf), "%H", &tstruct);

	return std::string(buf);
};
int saveSmokeToCSV() {
	std::fstream file;
	std::string filePath = "smoke/smoke_" + getCurrentDateTime("date") + ".csv";
	file.open(filePath, std::ios::ios_base::app);
	file << getCurrentDateTime("time") << "," << tunnelReadings[1];
	for (int j = 0; j < SENSORS; j++)
		if (sen[j].activated)
			file << "," << sen[j].SMOKE - sen[j].BASE;
	file << '\n';
	file.close();
	return 0;
}
int updateSmokeCSVHeader() { //check if smoke file open
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	std::fstream smokeFile;
	std::string filePath = "smoke/smoke_" + getCurrentDateTime("date") + ".csv";
	smokeFile.open(filePath, std::ios::ios_base::app);
	smokeFile << "time" << ",scatter";
	for (int i = 0; i < SENSORS; i++)
		if (sen[i].activated)
			smokeFile << "," << sen[i].serial;
	smokeFile << "\n";
	serialHeaderChanged = false;
	smokeFile.close();
	return 1;
}
int startCalibration(int test, int* calibrating, bool skipSql, std::string* previousTest, std::string* currentTest, bool* tunnelPurged) { //should merge the two cal initilizations but also allow for popups since it needs
	for (int i = 0; i < SENSORS; i++) { // to be on a detatched thread to not block the gui
		sen[i].testPassed = "N/A";
	}
	if (*calibrating == 1 || *calibrating == 2 || *calibrating == 3) {
		ImGui::OpenPopup(gettext("Test already in progress"));
		return 0;
	}
	if (!tunnelConnected) {
		*calibrating = 10;
		ImGui::OpenPopup(gettext("Tunnel not connected"));
		return 0;
	}
	if (test == 0) {
		ImGui::OpenPopup(gettext("Test not selected"));
		return 0;
	}
	if (tunnelReadings[6] == "0") {
		ImGui::OpenPopup(gettext("Door must be closed"));
		return 0;
	}
	if (stof(tunnelReadings[2]) < 15.0f || stof(tunnelReadings[2]) > 35.0f) { //check sql
		ImGui::OpenPopup(gettext("Tunnel temperature not in range"));
		return 0;
	}
	for (int i = 0; i < SENSORS; i++)
		if (!sen[i].fCTS && sen[i].COM != "" && sen[i].activated)
			ImGui::OpenPopup(gettext("Not receiving communications"));
	//if (tunnelReadings[4] != "00") {
	//	ImGui::OpenPopup("Test already in progress");
	//	return 0;
	//}

	if (test - 1 == 1) {//check if trays are the same type in alarm test
		int i;
		bool tray1IsEmpty = true, tray2IsEmpty = true;
		for (i = 0; i < 16; i++)
			if (sen[i].activated) {
				tray1IsEmpty = false;
				break;
			}
		int k;
		for (k = 16; k < SENSORS; k++)
			if (sen[k].activated) {
				tray2IsEmpty = false;
				break;
			}
		if (!tray1IsEmpty && !tray2IsEmpty)
			if ((bool)sen[i].deviceType != (bool)sen[k].deviceType) {
				ImGui::OpenPopup(gettext("Different tray types"));
				return 0;
			}
	}
	bool isSmartcell = true;
	for (int i = 0; i < 16; i++)
		if (sen[i].activated)
			isSmartcell = (bool)sen[i].deviceType;
	if (test - 1 == 0) {
		*currentTest = "calibration";
		if (*previousTest != "calibration" && *previousTest != "") {
			ImGui::OpenPopup("Tunnel must be emptied");
			*tunnelPurged = false;
			return 0;
		}

	}
	else if (test - 1 == 1) {
		*currentTest = isSmartcell ? "alarmSmartcell" : "alarmUTC";
		if (*previousTest != *currentTest && *previousTest != "") {
			ImGui::OpenPopup("Tunnel must be emptied");
			*tunnelPurged = false;
			return 0;
		}
	}

	int sensors = 0;
	for (int i = 0; i < SENSORS; i++)
		if (sen[i].activated == 1)
			sensors++;
	if (sensors == 0)
		ImGui::OpenPopup(gettext("No sensors detected"));
	else if (sensors < BATCH_SIZE)
		ImGui::OpenPopup(gettext("Less than 30 sensors detected"));
	else
		calibrateSensors(test, calibrating, skipSql);

	return 0;
}
int calibrateSensors(int test, int* calibrating, bool skipSql) {

	calibrationPresets presets[3];
	loadTestPresets(presets);

	if (presets[test - 1].min > presets[test - 1].max) {
		LOG_F(2, gettext("Calibration preset has minimum read value set higher than maximum read value"));
		*calibrating = 11;
		return 0;
	}
	if (presets[test - 1].min > 10 || presets[test - 1].min < 0) {
		LOG_F(2, gettext("Calibration preset has minimum range set at an extreme range (%.2f)"), presets[test - 1].min);
		*calibrating = 11;
		return 0;
	}
	if (presets[test - 1].max > 10 || presets[test - 1].max < 0) {
		LOG_F(2, gettext("Calibration preset has maximum range set at an extreme range (%.2f)"), presets[test - 1].max);
		*calibrating = 11;
		return 0;
	}
	if (presets[test - 1].samples > 1000 || presets[test - 1].samples < 5) {
		LOG_F(2, gettext("Calibration preset has too many or too few samples (%d)"), presets[test - 1].samples);
		*calibrating = 11;
		return 0;
	}

	if (presets[test - 1].preAlarmSetPoint > presets[test - 1].alarmSetPoint) {
		LOG_F(2, gettext("Calibration preset has pre alarm value set higher than alarm value"));
		*calibrating = 11;
		return 0;
	}
	bool isSmartcell = true;
	for (int i = 0; i < 16; i++)
		if (sen[i].activated)
			isSmartcell = (bool)sen[i].deviceType;

	if (test - 1 == 1) {
		if (isSmartcell) {
			tunnelCommands.push_back(presets[test - 1].tunnelCommand.c_str());
		}
		else {
			tunnelCommands.push_back(presets[test - 1].tunnelCommand2.c_str());
		}
	}
	else {
		tunnelCommands.push_back(presets[test - 1].tunnelCommand.c_str());
	}
	int sens = 0;
	for (int i = 0; i < SENSORS; i++) {
		sen[i].completedTest = -1;
		if (sen[i].activated)
			sens++;
	}

	std::thread worker[SENSORS];

	for (int i = 0; i < SENSORS; i++) {
		if (sen[i].activated) {
			if (presets[test - 1].testType == 0)
				worker[i] = std::thread(setAlarmThreshold, &sen[i], calibrating, presets[test - 1], sens, skipSql);
			//else if (presets[test - 1].testType == 1)
			//	worker[i] = std::thread(alarmClearTest, &sen[i], calibrating, presets[test - 1]);
			//else if (presets[test - 1].testType == 2)
			//	worker[i] = std::thread(alarmRampTest, &sen[i], calibrating, presets[test - 1]);
			else if (presets[test - 1].testType == 1)
				worker[i] = std::thread(alarmTest, &sen[i], calibrating, presets[test - 1], sens, skipSql);
			worker[i].detach();
		}
	}
	if (presets[test - 1].testType) {
		if (isSmartcell) {
			LOG_F(0, gettext("Starting alarm test (%s) for Smartcell"), presets[test - 1].testName.c_str());
			LOG_F(0, gettext("Smartcell min read set to %.3f and max read to %.3f with %d samples"), presets[test - 1].min_smartcell, presets[test - 1].max_smartcell, presets[test - 1].samples);
		}
		else {
			LOG_F(0, gettext("Starting alarm test (%s) for UTC"), presets[test - 1].testName.c_str());
			LOG_F(0, gettext("UTC min read set to %.3f and max read to %.3f with %d samples"), presets[test - 1].min_utc, presets[test - 1].max_utc, presets[test - 1].samples);
		}
	}
	else {
		LOG_F(0, gettext("Starting calibration (%s)"), presets[test - 1].testName.c_str());
		LOG_F(0, gettext("Smartcell pre alarm set to %.3f and alarm set to %.3f"), presets[test - 1].preAlarmSetPoint, presets[test - 1].alarmSetPoint);
		LOG_F(0, gettext("UTC pre alarm set to %.3f and alarm set to %.3f"), presets[test - 1].UTC_preAlarmSetPoint, presets[test - 1].UTC_alarmSetPoint);
		LOG_F(0, gettext("Reading from %.3f to %.3f with %d samples"), presets[test - 1].min, presets[test - 1].max, presets[test - 1].samples);

	}
	if (skipSql)
		LOG_F(0, gettext("Overrided failed/empty SQL queries"));
	return 1;

}
int stopCalibration(int* calibrating, std::string* previousTest, bool* tunnelEmptying) {
	if (*tunnelEmptying)
		return 1;
	else
		*tunnelEmptying = true;
	if (tunnelConnected) {
		*calibrating = 9;
		tunnelCommands.push_back("STP");
		std::this_thread::sleep_for(std::chrono::seconds(7));
		tunnelCommands.push_back("STP");
		tunnelCommands.push_back("STP");
		LOG_F(0, gettext("Tunnel emptied"));
		*tunnelEmptying = false;
	}
	else {
		*calibrating = 10;
	}
	return 1;

}

void TextCenter(const char* format, ...) {
	std::string text;
	char msg[150];
	va_list args;
	va_start(args, format);
	vsnprintf(msg, sizeof(msg), format, args); // do check return value
	va_end(args);
	text = msg;
	float font_size = ImGui::GetFontSize() * text.size() / 2;
	ImGui::Text("");
	ImGui::SameLine(
		ImGui::GetColumnWidth() / 2 -
		font_size + (font_size / 2)
	);

	ImGui::Text(text.c_str());
}
void TextCenterColored(const ImVec4& col, const char* format, ...) {
	std::string text;
	char msg[150];
	va_list args;
	va_start(args, format);
	vsnprintf(msg, sizeof(msg), format, args); // do check return value
	va_end(args);
	text = msg;
	float font_size = ImGui::GetFontSize() * text.size() / 2;
	ImGui::Text("");
	ImGui::SameLine(
		ImGui::GetColumnWidth() / 2 -
		font_size + (font_size / 2)
	);

	ImGui::TextColored(col, text.c_str());
}
void StyleSeaborn() {

	ImPlotStyle& style = ImPlot::GetStyle();

	ImVec4* colors = style.Colors;
	colors[ImPlotCol_Line] = IMPLOT_AUTO_COL;
	colors[ImPlotCol_Fill] = IMPLOT_AUTO_COL;
	colors[ImPlotCol_MarkerOutline] = IMPLOT_AUTO_COL;
	colors[ImPlotCol_MarkerFill] = IMPLOT_AUTO_COL;
	colors[ImPlotCol_ErrorBar] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
	colors[ImPlotCol_FrameBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.20f);
	//colors[ImPlotCol_PlotBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.40f);
	//colors[ImPlotCol_PlotBorder] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	//colors[ImPlotCol_LegendBg] = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
	//colors[ImPlotCol_LegendBorder] = ImVec4(0.80f, 0.81f, 0.85f, 1.00f);
	//colors[ImPlotCol_LegendText] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
	//colors[ImPlotCol_TitleText] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
	//colors[ImPlotCol_InlayText] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
	colors[ImPlotCol_XAxis] = ImVec4(0.00f, 0.00f, 0.00f, 0.20f);
	colors[ImPlotCol_XAxisGrid] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);

	colors[ImPlotCol_YAxis] = ImVec4(0.00f, 0.00f, 0.00f, 0.20f);
	colors[ImPlotCol_YAxisGrid] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

	//colors[ImPlotCol_YAxis2] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	//colors[ImPlotCol_YAxisGrid2] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);

	//colors[ImPlotCol_YAxis3] = ImVec4(0.00f, 1.00f, 0.00f, 1.00f);
	//colors[ImPlotCol_YAxisGrid3] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);

	colors[ImPlotCol_Selection] = ImVec4(1.00f, 0.65f, 0.00f, 1.00f);
	colors[ImPlotCol_Query] = ImVec4(0.23f, 0.10f, 0.64f, 1.00f);
	colors[ImPlotCol_Crosshairs] = ImVec4(0.23f, 0.10f, 0.64f, 0.50f);

	style.LineWeight = 1.5;
	style.Marker = ImPlotMarker_None;
	style.MarkerSize = 4;
	style.MarkerWeight = 1;
	style.FillAlpha = 1.0f;
	style.ErrorBarSize = 5;
	style.ErrorBarWeight = 1.5f;
	style.DigitalBitHeight = 8;
	style.DigitalBitGap = 4;
	style.PlotBorderSize = 1;
	style.MinorAlpha = 1.0f;
	style.MajorTickLen = ImVec2(0, 0);
	style.MinorTickLen = ImVec2(0, 0);
	style.MajorTickSize = ImVec2(10, 10);
	style.MinorTickSize = ImVec2(0, 0);
	style.MajorGridSize = ImVec2(1.2f, 1.2f);
	style.MinorGridSize = ImVec2(1.2f, 1.2f);
	style.PlotPadding = ImVec2(1, 1);
	style.LabelPadding = ImVec2(5, 5);
	style.LegendPadding = ImVec2(5, 5);
	style.MousePosPadding = ImVec2(5, 5);
	style.PlotMinSize = ImVec2(600, 225);
}
static void HelpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}
int loadTestPresets(calibrationPresets* presets) {
	int noPresets = 2;
	std::ifstream file;
	file.open("config/test_presets.txt");
	std::string line;
	char buffer[100];
	bool badRead = false;
	if (file) {
		int id = -1;
		while (std::getline(file, line)) {
			int n = 0;
			snprintf(buffer, 100, "preset %d:", id + 2);
			if (line.find(buffer) != -1) {
				id++;
				if (id == noPresets)
					break;
				else
					continue;
			}
			else if ((n = line.find("\ttest_name:")) == 0)
				presets[id].testName = line.substr(n + 11, 20);
			else if ((n = line.find("\ttunnel_command:")) == 0)
				presets[id].tunnelCommand = line.substr(n + 16, 20);
			else if ((n = line.find("\ttunnel_command_smartcell:")) == 0)
				presets[id].tunnelCommand = line.substr(n + 26, 20);
			else if ((n = line.find("\ttunnel_command_utc:")) == 0)
				presets[id].tunnelCommand2 = line.substr(n + 20, 20);
			else if ((n = line.find("\ttest_type:")) != -1) {
				presets[id].testType = std::stoi(line.substr(11, 1));
			}
			else if ((n = line.find("\tscatter_read:1")) != -1)
				presets[id].readFromScatter = true;
			else if ((n = line.find("\tscatter_read:0")) != -1)
				presets[id].readFromScatter = false;
			else if ((n = line.find("\tmin:")) != -1) {
				if (isFloatNumber(line.substr(n + 5, 5)))
					presets[id].min = std::stof(line.substr(n + 5, 5));
			}
			else if ((n = line.find("\tmax:")) != -1) {
				if (isFloatNumber(line.substr(n + 5, 5)))
					presets[id].max = std::stof(line.substr(n + 5, 5));
			}
			else if ((n = line.find("\tsamples:")) != -1) {
				if (isNumber(line.substr(n + 9, 3)))
					presets[id].samples = std::stoi(line.substr(n + 9, 3));
			}
			else if ((n = line.find("\tsmartcell_pre_alarm_set_point:")) != -1) {
				if (isFloatNumber(line.substr(n + 31, 5)))
					presets[id].preAlarmSetPoint = std::stof(line.substr(n + 31, 5));
			}
			else if ((n = line.find("\tsmartcell_alarm_set_point:")) != -1) {
				if (isFloatNumber(line.substr(n + 27, 5)))
					presets[id].alarmSetPoint = std::stof(line.substr(n + 27, 5));
			}
			else if ((n = line.find("\tutc_pre_alarm_set_point:")) != -1) {
				if (isFloatNumber(line.substr(n + 25, 5)))
					presets[id].UTC_preAlarmSetPoint = std::stof(line.substr(n + 25, 5));
			}
			else if ((n = line.find("\tutc_alarm_set_point:")) != -1) {
				if (isFloatNumber(line.substr(n + 21, 5)))
					presets[id].UTC_alarmSetPoint = std::stof(line.substr(n + 21, 5));
			}
			else if ((n = line.find("\tmin_smartcell:")) != -1) {
				if (isFloatNumber(line.substr(n + 15, 5)))
					presets[id].preAlarmSetPoint = std::stof(line.substr(n + 15, 5));
			}
			else if ((n = line.find("\tmax_smartcell:")) != -1) {
				if (isFloatNumber(line.substr(n + 15, 5)))
					presets[id].alarmSetPoint = std::stof(line.substr(n + 15, 5));
			}
			else if ((n = line.find("\tmin_utc:")) != -1) {
				if (isFloatNumber(line.substr(n + 9, 5)))
					presets[id].UTC_preAlarmSetPoint = std::stof(line.substr(n + 9, 5));
			}
			else if ((n = line.find("\tmax_utc:")) != -1) {
				if (isFloatNumber(line.substr(n + 9, 5)))
					presets[id].UTC_alarmSetPoint = std::stof(line.substr(n + 9, 5));
			}
		}

		if (badRead) {
			LOG_F(2, gettext("Corrupted/invalid test presets, generating new file"));
			file.close();
			createCalibrationPresets();
			return 0;
		}
		file.close();

	}
	else {
		LOG_F(2, gettext("Could not load test presets file"));
		return 0;
	}
	return 1;
}
int createCalibrationPresets() {
	int noPresets = 2;
	std::ofstream file;
	std::string filePath = "config/test_presets.txt";
	file.open(filePath, std::ios::ios_base::out);
	if (file) {
		for (int i = 1; i < noPresets + 1; i++) {
			file << "preset " << i << ":" << "\n";
			file << "\ttest_name:" << "Test " << i << "\n";
			file << "\ttunnel_command:" << "S0" << i << "\n";
			file << "\tscatter_read:" << "1" << "\n";
			file << "\ttest_type:" << "0" << "\n";
			file << "\tmin:" << "0.198" << "\n";
			file << "\tmax:" << "0.202" << "\n";
			file << "\tsamples:" << "30" << "\n";
			file << "\tsmartcell_pre_alarm_set_point:" << "0.100" << "\n";
			file << "\tsmartcell_alarm_set_point:" << "0.140" << "\n";
			file << "\tutc_pre_alarm_set_point:" << "0.100" << "\n";
			file << "\tutc_alarm_set_point:" << "0.140" << "\n";
		}
		file.close();
		return 1;
	}
	return 0;
}
int saveCalibrationPresets(calibrationPresets* presets) {
	int noPresets = 2;
	std::ofstream file;
	std::string filePath = "config/test_presets.txt";
	file.open(filePath, std::ios::ios_base::out);
	if (file) {
		for (int i = 1; i < noPresets + 1; i++) {
			file << "preset " << i << ":" << "\n";
			file << "\ttest_name:" << presets[i - 1].testName << "\n";
			file << "\tscatter_read:" << presets[i - 1].readFromScatter << "\n";
			file << "\ttest_type:" << presets[i - 1].testType << "\n";
			if (presets[i - 1].testType == 0) {
				file << "\ttunnel_command:" << presets[i - 1].tunnelCommand << "\n";
				file << "\tmin:" << presets[i - 1].min << "\n";
				file << "\tmax:" << presets[i - 1].max << "\n";
				file << "\tsamples:" << presets[i - 1].samples << "\n";
				file << "\tsmartcell_pre_alarm_set_point:" << presets[i - 1].preAlarmSetPoint << "\n";
				file << "\tsmartcell_alarm_set_point:" << presets[i - 1].alarmSetPoint << "\n";
				file << "\tutc_pre_alarm_set_point:" << presets[i - 1].UTC_preAlarmSetPoint << "\n";
				file << "\tutc_alarm_set_point:" << presets[i - 1].UTC_alarmSetPoint << "\n";
			}

			else if (presets[i - 1].testType == 1) {
				file << "\ttunnel_command_smartcell:" << presets[i - 1].tunnelCommand << "\n";
				file << "\ttunnel_command_utc:" << presets[i - 1].tunnelCommand2 << "\n";
				file << "\tmin_smartcell:" << presets[i - 1].min_smartcell << "\n";
				file << "\tmax_smartcell:" << presets[i - 1].max_smartcell << "\n";
				file << "\tmin_utc:" << presets[i - 1].min_utc << "\n";
				file << "\tmax_utc:" << presets[i - 1].max_utc << "\n";
				file << "\tsamples:" << presets[i - 1].samples << "\n";
			}
		}
		file.close();
		LOG_F(0, gettext("Saved test presets to file"));
	}
	else
		LOG_F(2, gettext("Could not save test presets to file"));

	return 0;
}
INT_PTR WINAPI WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	// Routine Description:
	//     Simple Windows callback for handling messages.
	//     This is where all the work is done because the example
	//     is using a window to process messages. This logic would be handled 
	//     differently if registering a service instead of a window.

	// Parameters:
	//     hWnd - the window handle being registered for events.

	//     message - the message being interpreted.

	//     wParam and lParam - extended information provided to this
	//          callback by the message sender.

	//     For more information regarding these parameters and return value,
	//     see the documentation for WNDCLASSEX and CreateWindowEx.
	LRESULT lRet = 1;
	static HDEVNOTIFY hDeviceNotify;
	static HWND hEditWnd;
	static ULONGLONG refresh = 0;

	GUID WceusbshGUID = { 0x25dbce51, 0x6c8f, 0x4a72,
						  0x8a,0x6d,0xb5,0x4c,0x2b,0x4f,0xc8,0x35 };

	switch (message)
	{
	case WM_CREATE:
		//
		// This is the actual registration., In this example, registration 
		// should happen only once, at application startup when the window
		// is created.
		//
		// If you were using a service, you would put this in your main code 
		// path as part of your service initialization.
		//
		if (!DoRegisterDeviceInterfaceToHwnd(WceusbshGUID, hWnd, &hDeviceNotify))
		{
			// Terminate on failure.
			ExitProcess(1);
		}

		break;

	case WM_DEVICECHANGE:
	{

		//PDEV_BROADCAST_DEVICEINTERFACE b = (PDEV_BROADCAST_DEVICEINTERFACE)lParam;
		switch (wParam)
		{
		case DBT_DEVNODES_CHANGED: {
				std::thread t1 = std::thread(COMSetup, false);
				t1.detach();
			break;
		}
		default:
			break;
		}
	}
	break;
	case WM_CLOSE:
		if (!UnregisterDeviceNotification(hDeviceNotify))
		{
		}
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		// Send all other messages on to the default windows handler.
		lRet = DefWindowProc(hWnd, message, wParam, lParam);
		break;
	}

	return lRet;
}
BOOL DoRegisterDeviceInterfaceToHwnd(IN GUID InterfaceClassGuid, IN HWND hWnd, OUT HDEVNOTIFY* hDeviceNotify)
// Routine Description:
//     Registers an HWND for notification of changes in the device interfaces
//     for the specified interface class GUID. 

// Parameters:
//     InterfaceClassGuid - The interface class GUID for the device 
//         interfaces.  

//     hWnd - Window handle to receive notifications.

//     hDeviceNotify - Receives the device notification handle. On failure, 
//         this value is NULL.

// Return Value:
//     If the function succeeds, the return value is TRUE.
//     If the function fails, the return value is FALSE.

// Note:
//     RegisterDeviceNotification also allows a service handle be used,
//     so a similar wrapper function to this one supporting that scenario
//     could be made from this template.
{
	DEV_BROADCAST_DEVICEINTERFACE NotificationFilter;

	ZeroMemory(&NotificationFilter, sizeof(NotificationFilter));
	NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
	NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	NotificationFilter.dbcc_classguid = InterfaceClassGuid;

	*hDeviceNotify = RegisterDeviceNotification(
		hWnd,                       // events recipient
		&NotificationFilter,        // type of device
		DEVICE_NOTIFY_WINDOW_HANDLE // type of recipient handle
	);

	if (NULL == *hDeviceNotify)
	{
		return FALSE;
	}

	return TRUE;
}
int closeHandleWrapper(sensorStruct* sensor) {
	if (!sensor->isClosed)
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	if (sensor->hSerial != INVALID_HANDLE_VALUE)
		CloseHandle(sensor->hSerial);
	else {
		sensor->deviceType = 4;
		sensor->activated = 0;
		sensor->fCTS = 0;
		sensor->disconnectedCounter++;
		sensor->serial = "";
	}
	sensor->isClosed = true;
	return 0;
}
int generateTraySetup() {
	std::ofstream file("config/COMs.txt");
	std::string tray_name[4] = { "UTC_A:", "UTC_B:", "SMARTCELL_A:", "SMARTCELL_B:" };
	file << "TunnelCOM:" << '\n';
	for (int i = 0; i < 4; i++) {
		file << tray_name[i] << '\n';
		for (int j = 0; j < SENSORS / 2; j++)
			file << "\tsensor " << j + 1 << ":" << '\n';
	}
	file.close();
	return 0;
}
int COMRescan() {
	char* portName[256];
	for (int i = 0; i < 256; i++)
		portName[i] = (char*)calloc(256, 1);
	int n = 0;
	run = false;
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	for (int i = 0; i < SENSORS; i++) {
		sen[i].COM = "";
		sen[i].serial = "";
		sen[i].activated = false;
		sen[i].disconnectedCounter = 0;
		sen[i].fCTS = 0;
		sen[i].deviceType = 4;
		sen[i].sdata.Erase();
		sen[i].sdata2.Erase();
	}
	LOG_F(0, gettext("Manually scanning for COMs"));
	EnumerateComPortQueryDosDevice(&n, portName);
	LOG_F(0, gettext("Found %d COMs"), n);
	int skip = 0;
	char buffer[100];
	for (int i = 0; i < n; i++) {
		snprintf(buffer, 100, "\\\\.\\%s", &portName[i][0]);
		if (portName[i][3] == '\0' || !strcmp(buffer, "\\\\.\\COM1") || !strcmp(buffer, tunnelCOM.c_str())) { //check, not sure why check for [i][3]
			skip++;
			continue;
		}
		sen[i - skip].COM = buffer;
	}

	for (int j = 0; j < SENSORS; j++)
		COM_selectable[j] = sen[j].COM.c_str();

	run = true;
	std::thread t = std::thread(updateSmokeCSVHeader);
	t.detach();
	return 0;
}
bool isNumber(const std::string& str)
{
	if (str == "")
		return false;
	try
	{
		int i = std::stoi(str);
	}
	catch (...)
	{
		return false;
	}
	for (char const& c : str) {
		if (std::isdigit(c) == 0) return false;
	}
	return true;
}
void tokenize(std::string& str, char delim, std::vector<std::string>& out)
{
	size_t start;
	size_t end = 0;

	while ((start = str.find_first_not_of(delim, end)) != std::string::npos)
	{
		end = str.find(delim, start);
		out.push_back(str.substr(start, end - start));
	}
}
bool isFloatNumber(const std::string& string) {
	std::string::const_iterator it = string.begin();
	bool decimalPoint = false;
	int minSize = 0;
	if (string.size() > 0 && (string[0] == '-' || string[0] == '+')) {
		it++;
		minSize++;
	}
	while (it != string.end()) {
		if (*it == '.') {
			if (!decimalPoint) decimalPoint = true;
			else break;
		}
		else if (!std::isdigit(*it) && ((*it != 'f') || it + 1 != string.end() || !decimalPoint)) {
			break;
		}
		++it;
	}
	return string.size() > minSize && it == string.end();
}
int changeLanguage(int lang) {
	//gnu_gettext::messages_info info;

	//if (lang) {
	//	info.language = "pl";
	//	info.country = "PL";
	//	info.encoding = "CP1252";

	//}
	//else {
	//	info.language = "en";
	//	info.country = "GB";
	//	info.encoding = "UTF-8";

	//}
	//info.paths.push_back("./");
	//info.domains.push_back(gnu_gettext::messages_info::domain("messages"));
	//info.callback = file_loader();
	//std::locale l(std::locale::classic(), boost::locale::gnu_gettext::create_messages_facet<char>(info));
	//std::locale::global(l);

	return 0;
}
int openPorts(sensorStruct* sensor) {

	if (sensor->deviceType && !sensor->openingPorts) {
		sensor->openingPorts = true;
		sensor->queue.push_back("PORTG2111");
		sensor->queue.push_back("PORTG1110");
		sensor->queue.push_back("PORTE4111");
		sensor->queue.push_back("PORTB0110");
		sensor->queue.push_back("PORTG1111");
	}
	return 0;
}
std::vector<std::string> ExecuteSql(const CHAR* sql, int* status) {
	std::vector<std::string> results = {};
	HENV    hEnv = NULL;
	HDBC    hDbc = NULL;
	HSTMT hStmt = NULL;
	int iConnStrLength2Ptr;
	CHAR szConnStrOut[256];
	SQLINTEGER rowCount = 0;
	SQLSMALLINT fieldCount = 0;
	SQLCHAR buf[2048];
	SQLLEN ret;

	/* ODBC API return status */
	RETCODE rc;

	/* Allocate an environment handle */
	rc = SQLAllocEnv(&hEnv);
	/* Allocate a connection handle */
	rc = SQLAllocConnect(hEnv, &hDbc);

	/* Connect to the database */
	rc = SQLDriverConnect(hDbc,
		NULL,
		//(SQLCHAR*)"DRIVER={SQL Server};Server=localhost\\SQLEXPRESS;Database=master;Trusted_Connection=True;",
		(SQLCHAR*)"SERVER=10.237.217.144;PORT=3306;UID=testjig;PWD=testjig123;DRIVER={MySQL ODBC 8.0 Unicode Driver};",
		SQL_NTS,
		buf,
		1024,
		NULL,
		SQL_DRIVER_NOPROMPT);

	if (SQL_SUCCEEDED(rc))
	{
		/* Prepare SQL query */
		rc = SQLAllocStmt(hDbc, &hStmt);
		rc = SQLPrepare(hStmt, (SQLCHAR*)sql, SQL_NTS);

		/* Excecute the query */
		rc = SQLExecute(hStmt);
		if (SQL_SUCCEEDED(rc))
		{
			SQLNumResultCols(hStmt, &fieldCount);

			if (fieldCount > 0)
			{
				/* Loop through the rows in the result set */

				rc = SQLFetch(hStmt);
				int col = 0;
				while (SQL_SUCCEEDED(rc))
				{
					//get data
					rc = SQLGetData(hStmt, col + 1, SQL_C_CHAR, buf, sizeof(buf), &ret);

					if (SQL_SUCCEEDED(rc) == FALSE) {
						*status = -1;
						LOG_F(2, "SQLGetData failed for query: %s", sql);
						continue;
					}

					//convert data to string
					std::string str;
					if (ret <= 0) {
						str = "(null)";
					}
					else {
						str = std::string((char*)buf);
					}

					results.push_back(str);
					col++;
					if (col == fieldCount) {
						rc = SQLFetch(hStmt);
						rowCount++;
						col = 0;

					}
				};

				rc = SQLFreeStmt(hStmt, SQL_DROP);

			}
			//else
			//{
			//	LOG_F(2, "Error: Number of fields in the result set is 0.\n");
			//}

		}
		else {
			LOG_F(2, "Could not execute SQL query: %s", sql);
			*status = -1;
			DisplayError(SQL_HANDLE_STMT, hStmt);
		}
	}
	else
	{
		*status = -1;
		LOG_F(2, "Couldn't connect to SQL server");
		DisplayError(SQL_HANDLE_DBC, hDbc);
	}

	/* Disconnect and free up allocated handles */
	SQLDisconnect(hDbc);
	SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
	SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
	return results;
}
void DisplayError(SQLSMALLINT t, SQLHSTMT h) {
	SQLCHAR       SqlState[6], Msg[SQL_MAX_MESSAGE_LENGTH];
	SQLINTEGER    NativeError;
	SQLSMALLINT   i, MsgLen;
	SQLRETURN     rc;

	SQLLEN numRecs = 0;
	SQLGetDiagField(t, h, 0, SQL_DIAG_NUMBER, &numRecs, 0, 0);

	// Get the status records.
	i = 1;
	while (i <= numRecs && (rc = SQLGetDiagRec(t, h, i, SqlState, &NativeError,
		Msg, sizeof(Msg), &MsgLen)) != SQL_NO_DATA) {
		LOG_F(2, "Error %d: %s", NativeError, Msg);
		i++;
	}

}
void GetMachineName(char machineName[150])
{
	int i = 0;
	TCHAR infoBuf[150];
	DWORD bufCharCount = 150;
	memset(machineName, 0, 150);
	if (GetComputerName(infoBuf, &bufCharCount))
	{
		for (i = 0; i < 150; i++)
		{
			machineName[i] = infoBuf[i];
		}
	}
	else
	{
		strcpy(machineName, "Unknown_Host_Name");
	}
}
int alarmTest(sensorStruct* sensor, int* status, calibrationPresets preset, int noSensors, bool skipSql) {
	sensor->completedTest = 0;
	bool isSmartcell = (bool)sensor->deviceType;

	*status = 20;
	LOG_F(0, gettext("[%s] Starting alarm test for sensor %d [%s] "), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	Idle_Time_End = std::chrono::steady_clock::now();
	int timeout = 0;
	int samples = preset.samples;
	int readFromScatter = preset.readFromScatter;
	int consecutiveRead = 0;
	float rangeMin = isSmartcell ? preset.min_smartcell : preset.min_utc;
	float rangeMax = isSmartcell ? preset.max_smartcell : preset.max_utc;
	//----sql-----
	float ALARM_AIR_SPEED = 0.0f;
	float ALARM_BASLINE = 0.0f;
	float ALARM_CHAMBER_POSITION = 0.0f;
	float ALARM_CLEAR_HEAT_VALUE_1 = 0.0f;
	float ALARM_CLEAR_HEAT_VALUE_2 = 0.0f;
	float ALARM_CLEAR_HEAT_VALUE_3 = 0.0f;
	float ALARM_CLEAR_OBSCURATION_VALUE = 0.0f;
	std::string ALARM_CLEAR_READ = "";
	float ALARM_CLEAR_SMOKE_VALUE = 0.0f;
	float ALARM_CLEAR_TEMPERATURE = 0.0f;
	float ALARM_OBSCURATION_VALUE = 0.0f;
	std::string ALARM_READ = "";
	int ALARM_RESULT = 0;
	float ALARM_SMOKE_VALUE = 0.0f;
	float ALARM_TEMPERATURE = 0.0f;

	std::string Assembled_Ident_Number = "";
	std::string CheckSum = "0";
	std::string DateTime_of_Test = "";
	float Idle_Time = 0.0f;
	int Mainboard_ID = 0;
	int Pass_Parameter_Set_Number = 0;
	int Record_Number = 0;
	std::string Revision = "";
	std::string Test_Jig_Software_Name = "";
	std::string Test_Jig_Software_Version = "";
	float Test_Time = 0.0f;
	int Tests_Passed = 0;
	std::string Type_ID = "";
	int User_ID = 0;
	std::string Works_Order_Number = "";
	char Workstation_Name[150];

	bool alarmClearStatus = 0;
	bool alarmStatus = 0;
	std::string database;

	std::vector<std::string> results;

	int sqlStatus = 0;
	bool sqlFailed = false;
	if (sensor->deviceType) {
		*status = 15;
		openPorts(sensor);
	}
	if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
		return 0;

	if (sensor->deviceType) {
		sensor->Assembled_Ident_Number = "S0" + sensor->serial;
		database = "tracking_smartcell";
	}
	else {
		sensor->Assembled_Ident_Number = "U0" + sensor->serial;
		database = "tracking_utc";
	}
	char buffer[2048];
	if (!sqlFailed) {
		snprintf(buffer, sizeof(buffer), "SELECT Assembled_Devices.Type_ID,\
										Assembled_Devices.Assembled_Ident_Number,\
										Assembled_Devices.Decommissioned,\
										Assembled_Devices.Record_Number,\
										Assembled_Devices.Works_Order_Number,\
										Assembled_Devices.Revision,\
										PCB_Software.Current_Software_ID,\
										PCB_Software.Processor_Number,\
										Software.Description,\
										Software.Options_File_Path,\
										Software.Version,\
										Software.File_Path_Name\
			FROM %s.Assembled_Devices LEFT OUTER JOIN %s.PCB_Software ON\
			PCB_Software.Type_ID = Assembled_Devices.Type_ID AND\
			PCB_Software.Revision = Assembled_Devices.Revision\
			LEFT OUTER JOIN %s.Software on Software.Software_ID = PCB_Software.Current_Software_ID\
			WHERE Assembled_Devices.DateTime_Assembled > '2021-02-24 11:03:29'AND \
			Assembled_Devices.Assembled_Ident_Number = '%s' ORDER BY\
			Assembled_Devices.Record_Number DESC LIMIT 1;", database.c_str(), database.c_str(), database.c_str(), sensor->Assembled_Ident_Number.c_str());
		results = ExecuteSql(buffer, &sqlStatus);
		if (sqlStatus == -1)
			sqlFailed = true;
		if (!results.size()) {
			snprintf(buffer, sizeof(buffer), "SELECT Assembled_Devices.Type_ID,\
										Assembled_Devices.Assembled_Ident_Number,\
										Assembled_Devices.Decommissioned,\
										Assembled_Devices.Record_Number,\
										Assembled_Devices.Works_Order_Number,\
										Assembled_Devices.Revision,\
										PCB_Software.Current_Software_ID,\
										PCB_Software.Processor_Number,\
										Software.Description,\
										Software.Options_File_Path,\
										Software.Version,\
										Software.File_Path_Name\
			FROM %s.Assembled_Devices LEFT OUTER JOIN %s.PCB_Software ON\
			PCB_Software.Type_ID = Assembled_Devices.Type_ID AND\
			PCB_Software.Revision = Assembled_Devices.Revision\
			LEFT OUTER JOIN %s.Software on Software.Software_ID = PCB_Software.Current_Software_ID\
			WHERE Assembled_Devices.Assembled_Ident_Number = '%s' ORDER BY\
			Assembled_Devices.Record_Number DESC LIMIT 1;", database.c_str(), database.c_str(), database.c_str(), sensor->Assembled_Ident_Number.c_str());
			results = ExecuteSql(buffer, &sqlStatus);
			if (sqlStatus == -1)
				sqlFailed = true;
		}

		if (results.size() > 5) {
			sensor->Type_ID = results[0];
			sensor->Decommissioned = stoi(results[2]);
			sensor->Works_Order_Number = results[4];
			sensor->Revision = results[5];

		}
		else
		{
			LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Assembled_Devices query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			sqlFailed = true;
		}
		if (sensor->Decommissioned) {
			LOG_F(2, "[%s] Sensor %d [%s] is decommissioned, cannot calibrate", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			sensor->completedTest = 1;
			return 1;
		}
		if (!sqlFailed) {
			snprintf(buffer, sizeof(buffer), "SELECT Device_Type, Brand_ID, Equivalent_Type_ID FROM %s.Device_Type WHERE Type_ID = '%s'", database.c_str(), sensor->Type_ID.c_str());
			results = ExecuteSql(buffer, &sqlStatus);
			if (sqlStatus == -1)
				sqlFailed = true;
			if (results.size() > 2) {
				sensor->Device_Type = results[0];
				sensor->Brand_ID = results[1];
				sensor->Equivalent_Type_ID = results[2];
			}
			else {
				sqlFailed = true;
				LOG_F(2, "[%s] Sensor %d [%s]: Could not fetch Device_Type query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
			}

			snprintf(buffer, sizeof(buffer), "SELECT EFACS_Part_Number FROM %s.EFACS_Part_Number WHERE Type_ID = '%s'", database.c_str(), sensor->Type_ID.c_str());
			results = ExecuteSql(buffer, &sqlStatus);
			if (sqlStatus == -1)
				sqlFailed = true;
			if (results.size())
				sensor->EFACS_Part_Number = results[0];
			else {
				LOG_F(2, "[%s] Sensor %d [%s]: Could not fetch EFACS_Part_Number query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
				sqlFailed = true;
			}

			snprintf(buffer, sizeof(buffer), "SELECT Mainboard_ID, Tests_Passed FROM %s.Radiated_Test_Stage\
			WHERE Assembled_Ident_Number = '%s' ORDER BY datetime_of_test DESC LIMIT 1", database.c_str(), sensor->Assembled_Ident_Number.c_str());
			results = ExecuteSql(buffer, &sqlStatus);
			if (sqlStatus == -1)
				sqlFailed = true;
			if (results.size() > 1) {
				sensor->Mainboard_ID = results[0];
				sensor->Tests_Passed = stoi(results[1]);
			}
			else {
				LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Radiated_Test_Stage query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
				sqlFailed = true;
			}

			if (!sensor->Tests_Passed) {
				LOG_F(2, "[%s] Sensor %d [%s] Previous test not passed, could not calibrate", sensor->serial.c_str(), SEN_ID, SEN_TRAY); //check notify
				sensor->testPassed = "PREVIOUS TEST NOT PASSED";
				sensor->completedTest = 1;
				return 1;
			}


			snprintf(buffer, sizeof(buffer), "SELECT Mainboard_ID, Tests_Passed, Checksum FROM %s.Calibration_Test_Stage\
			WHERE Assembled_Ident_Number = '%s' ORDER BY datetime_of_test DESC LIMIT 1", database.c_str(), sensor->Assembled_Ident_Number.c_str());
			results = ExecuteSql(buffer, &sqlStatus);
			if (sqlStatus == -1)
				sqlFailed = true;
			if (results.size() > 2) {
				sensor->Mainboard_ID = results[0];
				sensor->Tests_Passed = stoi(results[1]);
				CheckSum = results[2];
			}
			else {
				LOG_F(2, "[%s] Sensor %d [%s]: Could not fetch Calibration_Test_Stage query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
				sqlFailed = true;
			}

			if (!sensor->Tests_Passed) {
				LOG_F(2, "[%s] Sensor %d [%s]: Did not pass Calibration Test Stage", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
				sensor->testPassed = "PREVIOUS TEST NOT PASSED";
				sensor->completedTest = 1;
				return 1;
			}
		}
		if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
			return 0;

		//std::string Lower_Pass_Value = "0";
		//std::string Upper_Pass_Value = "0";
		//std::string Test_Name = "0";
		//std::string Field_Name = "0";
		//snprintf(buffer, sizeof(buffer), "SELECT Test_Stage_Pass_Parameters.Field_Name, \
		//	Test_Stage_Pass_Parameters.Lower_Pass_Value,\
		//	Test_Stage_Pass_Parameters.Upper_Pass_Value,\
		//	Test_Stage_Pass_Parameters.Type_ID,\
		//	Test_Stage_Pass_Parameters.Revision,\
		//	Test_Stage_Pass_Parameters.Pass_Parameter_Set_Number,\
		//	Test_Stage_Pass_Parameters.Test_Name\
		//	FROM %s.Test_Stages LEFT OUTER JOIN %s.Test_Stage_Pass_Parameters ON\
		//	Test_Stage_Pass_Parameters.Type_ID = Test_Stages.Type_ID WHERE Test_Stage_Pass_Parameters.Test_Name = 'alarm test' AND\
		//	Test_Stage_Pass_Parameters.Type_ID = '%s' AND Test_Stage_Pass_Parameters.Revision = '%s'", database.c_str(), database.c_str(), sensor->serial.c_str(), sensor->Revision.c_str());
		//results = ExecuteSql(buffer, &sqlStatus);
		//if (sqlStatus == -1)
		//	sqlFailed = true;
		//if (results.size() > 5) {
		//	Field_Name = results[0];
		//	Lower_Pass_Value = results[1];
		//	Upper_Pass_Value = results[2];
		//	Pass_Parameter_Set_Number = stoi(results[5]);
		//	Test_Name = results[6];
		//}
		//else {
		//	LOG_F(2, "[%s] Sensor %d [%s] Could not fetch Test_Stage_Pass_Parameters query", sensor->serial.c_str(), SEN_ID, SEN_TRAY);
		//	sqlFailed = true;
		//}
	}

	if (sqlFailed && !skipSql) {
		sensor->testPassed = "MISSING SQL QUERIES";
		sensor->completedTest = 1;
		return 0;
	}


	std::this_thread::sleep_for(std::chrono::seconds(6));

	if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
		return 0;
	else
		*status = 1;
	timeout = 720;

	// ====alarm====

	consecutiveRead = 0;
	while (timeout > 0) {
		if (sensor->completedTest == 1 || sensor->completedTest == -1) // tunnel disconnected or user cancelled 
			return 0;

		*status = 22;
		if (isFloatNumber(tunnelReadings[1])) {
			if (stof(tunnelReadings[readFromScatter]) >= rangeMin && stof(tunnelReadings[readFromScatter]) <= rangeMax) {
				consecutiveRead++;
				*status = 23;
				//AVERAGE_DEVICE_COUNT += sensor->SMOKE;
				ALARM_AIR_SPEED += stof(tunnelReadings[3]);
				ALARM_SMOKE_VALUE += sensor->SMOKE;
				ALARM_TEMPERATURE += stof(tunnelReadings[2]);
				ALARM_OBSCURATION_VALUE += stof(tunnelReadings[0]);

				if (sensor->SMOKE_STATUS == 2) {
					alarmStatus = 1;
					break;
				}
				if (consecutiveRead == samples)
					break;
			}
			else {
				consecutiveRead = 0;
				ALARM_AIR_SPEED = 0;
				ALARM_SMOKE_VALUE = 0;
				ALARM_TEMPERATURE = 0;
				ALARM_OBSCURATION_VALUE = 0;
			}
		}

		float milliseconds = 1000 / REFRESH_RATE;
		std::this_thread::sleep_for(std::chrono::milliseconds((int)milliseconds));
		timeout--;//check
	}
	if (!timeout) {
		sensor->completedTest = 1;
		*status = 12;
		return 0;
	}


	ALARM_AIR_SPEED /= consecutiveRead;
	ALARM_SMOKE_VALUE /= consecutiveRead;
	ALARM_TEMPERATURE /= consecutiveRead;
	ALARM_OBSCURATION_VALUE /= consecutiveRead;



	if (!alarmStatus) {
		LOG_F(2, gettext("[%s] Sensor %d [%s]: Failed alarm test"), sensor->serial.c_str(), SEN_ID, SEN_TRAY);
		sensor->testPassed = "FAILED";
	}
	else {
		ALARM_RESULT = 1;

		LOG_F(0, gettext("[%s] Sensor %d [%s]: Passed alarm test"),
			sensor->serial.c_str(), SEN_ID, SEN_TRAY);
		sensor->testPassed = "PASSED";
		Tests_Passed = 1;
	}
	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

	Test_Time = std::chrono::duration_cast<std::chrono::seconds>(end - begin).count();

	Idle_Time = std::chrono::duration_cast<std::chrono::seconds>(Idle_Time_End - Idle_Time_Begin).count();

	//ALARM_AIR_SPEED;
	ALARM_BASLINE = sensor->BASE;
	ALARM_CHAMBER_POSITION = sensor->id + 1;
	ALARM_CLEAR_HEAT_VALUE_1 = 0;
	ALARM_CLEAR_HEAT_VALUE_2 = 0;
	ALARM_CLEAR_HEAT_VALUE_3 = 0;
	ALARM_CLEAR_OBSCURATION_VALUE = 0;
	ALARM_CLEAR_READ = "Clear";//(alarmClearStatus == 1) ? "Alarm" : "Clear";
	ALARM_CLEAR_SMOKE_VALUE = 0;
	ALARM_CLEAR_TEMPERATURE = 0.0f;
	//ALARM_OBSCURATION_VALUE;
	ALARM_READ = alarmStatus ? "Alarm" : "Clear";
	//ALARM_RESULT;
	//ALARM_SMOKE_VALUE = 0.0f;
	//ALARM_TEMPERATURE = 0.0f;

	Assembled_Ident_Number = sensor->Assembled_Ident_Number;
	//CheckSum = "";
	DateTime_of_Test = getCurrentDateTime("now");
	//Idle_Time = 0.0f;
	Mainboard_ID = isNumber(sensor->Mainboard_ID) ? std::stoi(sensor->Mainboard_ID) : 0;
	//Pass_Parameter_Set_Number;
	//Record_Number;
	Revision = sensor->Revision;
	Test_Jig_Software_Name = SOFTWARE_NAME;
	Test_Jig_Software_Version = VERSION;
	//Test_Time;
	//Tests_Passed;
	Type_ID = sensor->Type_ID;
	User_ID = isNumber(user.serial) ? stoi(user.serial) : 0;
	Works_Order_Number = sensor->Works_Order_Number;
	GetMachineName(Workstation_Name);


	if (sensor->deviceType)
		snprintf(buffer, sizeof(buffer), "INSERT INTO %s.ALARM_TEST_STAGE(ALARM_AIR_SPEED, ALARM_BASELINE, ALARM_CHAMBER_POSITION, ALARM_CLEAR_HEAT_VALUE_1, ALARM_CLEAR_HEAT_VALUE_2, ALARM_CLEAR_HEAT_VALUE_3,\
			ALARM_CLEAR_OBSCURATION_VALUE, ALARM_CLEAR_READ, ALARM_CLEAR_SMOKE_VALUE, ALARM_CLEAR_TEMPERATURE, ALARM_OBSCURATION_VALUE, ALARM_READ, ALARM_RESULT, ALARM_SMOKE_VALUE,ALARM_TEMPERATURE,\
			Assembled_Ident_Number, CheckSum, DateTime_Of_Test, Idle_Time, Mainboard_ID, Pass_Parameter_Set_Number, Revision, Test_Jig_Software_Name, Test_Jig_Software_Version, \
		Test_Time, Tests_Passed, Type_ID, User_ID, Works_Order_Number, Workstation_Name)\
		Values (%.5f, %.5f,%.5f,%.5f,%.5f,%.5f,%.5f,'%s',%.5f,%.5f,%.5f,'%s',%d,%.5f,%.5f,'%s','%s','%s',%.5f,%d,%d,'%s','%s','%s',%.5f,%d,'%s',%d,'%s','%s');", database.c_str(), ALARM_AIR_SPEED, ALARM_BASLINE, ALARM_CHAMBER_POSITION,
			ALARM_CLEAR_HEAT_VALUE_1, ALARM_CLEAR_HEAT_VALUE_2, ALARM_CLEAR_HEAT_VALUE_3, ALARM_CLEAR_OBSCURATION_VALUE, ALARM_CLEAR_READ.c_str(), ALARM_CLEAR_SMOKE_VALUE, ALARM_CLEAR_TEMPERATURE, ALARM_OBSCURATION_VALUE, ALARM_READ.c_str(),
			ALARM_RESULT, ALARM_SMOKE_VALUE, ALARM_TEMPERATURE, Assembled_Ident_Number.c_str(), CheckSum.c_str(), DateTime_of_Test.c_str(), Idle_Time, Mainboard_ID, Pass_Parameter_Set_Number, Revision.c_str(),
			Test_Jig_Software_Name.c_str(), Test_Jig_Software_Version.c_str(), Test_Time, Tests_Passed, Type_ID.c_str(), User_ID, Works_Order_Number.c_str(), Workstation_Name);
	else
		snprintf(buffer, sizeof(buffer), "INSERT INTO %s.ALARM_TEST_STAGE(ALARM_AIR_SPEED, ALARM_BASELINE, ALARM_CHAMBER_POSITION, ALARM_CLEAR_HEAT_VALUE_1, ALARM_CLEAR_HEAT_VALUE_2, ALARM_CLEAR_HEAT_VALUE_3,\
			ALARM_CLEAR_OBSCURATION_VALUE, ALARM_CLEAR_READ, ALARM_CLEAR_SMOKE_VALUE, ALARM_CLEAR_TEMPERATURE, ALARM_OBSCURATION_VALUE, ALARM_READ, ALARM_SMOKE_VALUE,ALARM_TEMPERATURE,\
			Assembled_Ident_Number, CheckSum, DateTime_Of_Test, Idle_Time, Mainboard_ID, Pass_Parameter_Set_Number, Revision, Test_Jig_Software_Name, Test_Jig_Software_Version, \
		Test_Time, Tests_Passed, Type_ID, User_ID, Works_Order_Number, Workstation_Name)\
		Values (%.5f, %.5f,%.5f,%.5f,%.5f,%.5f,%.5f,'%s',%.5f,%.5f,%.5f,'%s',%.5f,%.5f,'%s','%s','%s',%.5f,%d,%d,'%s','%s','%s',%.5f,%d,'%s',%d,'%s','%s');", database.c_str(), ALARM_AIR_SPEED, ALARM_BASLINE, ALARM_CHAMBER_POSITION,
			ALARM_CLEAR_HEAT_VALUE_1, ALARM_CLEAR_HEAT_VALUE_2, ALARM_CLEAR_HEAT_VALUE_3, ALARM_CLEAR_OBSCURATION_VALUE, ALARM_CLEAR_READ.c_str(), ALARM_CLEAR_SMOKE_VALUE, ALARM_CLEAR_TEMPERATURE, ALARM_OBSCURATION_VALUE, ALARM_READ.c_str(),
			ALARM_SMOKE_VALUE, ALARM_TEMPERATURE, Assembled_Ident_Number.c_str(), CheckSum.c_str(), DateTime_of_Test.c_str(), Idle_Time, Mainboard_ID, Pass_Parameter_Set_Number, Revision.c_str(),
			Test_Jig_Software_Name.c_str(), Test_Jig_Software_Version.c_str(), Test_Time, Tests_Passed, Type_ID.c_str(), User_ID, Works_Order_Number.c_str(), Workstation_Name);
		results = ExecuteSql(buffer, &sqlStatus);
	if (sqlStatus == -1) {
		sensor->testPassed == "SQL INSERT FAILED";
		sensor->completedTest = 1;
		return 1;
	}

	sensor->completedTest = 1;
	timeout = 60;

	while (timeout) {
		int completed = 0;
		for (int i = 0; i < SENSORS; i++) {
			if (sen[i].completedTest == 1)
				completed++;
		}
		if (completed == noSensors) {
			if (*status != 24) {
				*status = 25;
				Idle_Time_Begin = std::chrono::steady_clock::now();

			}
			break;
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		timeout--;
	}
	if (!timeout)
		LOG_F(2, "Timeout occurred on counting completed sensors");//timeout message box
	return 1;
}
template< typename T >
std::string int_to_hex(T i)
{
	std::stringstream stream;
	stream << std::setfill('0') << std::setw(sizeof(T) * 2)
		<< std::hex << i;
	return stream.str();
}