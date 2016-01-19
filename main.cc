 /*
 * bluetooth remote control stuff.
 *
 * it's messy but it works and it's fast.
 *
 * depends on blue-z headers and hci.c as well as
 * microwindows with some patches. also needs the C++ standard
 * library and (obviously) libc. no other dependencies.
 *
 * this expects to be ran as PID1 and as root. logs on UART
 * and displays the UI on fb0. input done through USB HID
 * keyboards.
 *
 */

#include "INI.h"
typedef INI<> ini_t;

#define DEBUG_HID_STUFF 1
#define TOUCHSCREEN_ENABLED 0

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <error.h>
#include <assert.h>

/* linux */
#include <linux/input.h>
#include <linux/kd.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/reboot.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

/* stdc++ */
#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>

/* microwindows */
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <windows.h>
#include <device.h>

#include <bluetooth.h>
#include <hci.h>
#include <hci_lib.h>
#include <rfcomm.h>

#include <stdexcept>

/* context stuff for pairing */
#include "bt_ctx.h"

/* uhhhh */
extern "C" {
	#include <wintools.h>
}
using namespace std;

typedef int dir_mask_t;

#define DIRECTION_UP 0x1
#define DIRECTION_DOWN 0x2
#define DIRECTION_LEFT 0x4
#define DIRECTION_RIGHT 0x8

string g_PIN("1234");
int    g_Channel(1);
int    g_UDelay(1000);
int    g_IgnoreWeirdAppleKeyboardIssue(0);

struct ReplayCommand {
	unsigned cmd;
	unsigned sleep;
};
vector<ReplayCommand> g_TurnSequence;

extern int wait_for_pairing(bt_ctx* ctx);
extern void configure_adapter(int dd, int devnum);

extern "C"
int strnicmp(const char *s1, const char *s2, size_t len)
{
	return strncasecmp(s1, s2, len);
}

/* RAII */

struct free_scope {
	void* ptr;

	free_scope(void* ptr) : ptr(ptr) {}

	~free_scope() {
		free(ptr);
	}
};

struct close_scope {
	int& fd;
	bool ign;

	close_scope(int& fd) : fd(fd) { ign = false; }

	void ignore() {
		ign = true;
	}

	~close_scope() {
		if (!ign) {
			close(fd);
			fd = -1;
		}
	}
};

struct Directory {
	string path;

	Directory(string path) {
		this->path = path;
	}

	vector<string> files() {
		DIR           *d;
		struct dirent *dir;
		vector<string> vc;

		d = opendir(path.c_str());
		if (d)
		{
			while ((dir = readdir(d)) != NULL)
			{
				if (dir->d_type == DT_DIR)
					vc.push_back(string(dir->d_name) + "/");
				else
					vc.push_back(string(dir->d_name));
			}

			closedir(d);
		}

		return vc;
	}
};

/*
 * the BT stuff is hellishly ugly. this is (sortof) how it works
 *
 * init:
 * 1). scan for BT HCI devs, and bring them up (through HCIDEVUP)
 * 2). open the HCI dev and create BTDevice for it
 *
 * scan:
 * 1). hci_inquiry to run the scan
 * 2). hci_read_remote_nameblahblah to read remote device name
 *
 * connect:
 * 1). HCI CC/HCI auth done through embedded HCId
 * 2). open RFCOMM on the channel
 *
 * using SDP would be cleaner than just hardcoding the channel
 * but it's seriously too much effort.
 *
 * BT init is redone at scan time and persists until next scan.
 */
namespace Bluetooth {
	struct RFCOMMDevice {

	};

	enum move_cmd_t {
		kStop = 0x00,
		kForward = 0x10,
		kBackward = 0x20,
		kLeft = 0x30,
		kRight = 0x40,
		kLeftForward = 0x50,
		kRightForward = 0x60,
		kLeftBackward = 0x70,
		kRightBackward = 0x80
	};

	static const char* command_names[] = {
		"Stop",
		"Forward",
		"Backward",
		"Left",
		"Right",
		"LeftForward",
		"RightForward",
		"LeftBackward",
		"RightBackward"
	};

	struct Car {
		char speed;
		int socket;

		atomic_int txcmd;
		atomic_bool tx_thread_exited;

		thread tx_thread;

		Car() : speed(0) {
			txcmd = 0;
			tx_thread_exited = false;
		}

		~Car() {
			txcmd.store(-1);
		}

		int connect(string addr) {
			struct sockaddr_rc laddr;

			laddr.rc_family = AF_BLUETOOTH;
			laddr.rc_channel = g_Channel;

			/* ba */
			assert(!str2ba(addr.c_str(), &laddr.rc_bdaddr));

			socket = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);

			int r = ::connect(socket, (struct sockaddr*)&laddr, sizeof(laddr));
			if (r) {
				cout << "[CAR]     Failed to connect: " << strerror(errno) << endl;
				return -errno;
			}
			else {
				/* XXX this is totally stupid */
				thread([&](){
					cout << "car tx thread started!" << endl;
					while(1) {				
						int cmdl = txcmd.load();

						if (cmdl == -1)
							return;

						ssize_t s= sizeof(uint8_t);
						uint8_t zc = static_cast<uint8_t>(cmdl);
						//if (cmdl) {
							s = write(socket, &zc, sizeof(uint8_t));
							if (g_UDelay != 0)
								usleep(g_UDelay);
						//}

						if (s != sizeof(uint8_t)) {
							tx_thread_exited.store(true);
							cout <<"tx thread write err"<<endl;
							return;
						}
					}
					tx_thread_exited.store(true);
					cout << "car tx thread terminating!" << endl;
				}).detach();
			}

			return 0;
		}

		void run_stream() {

		}

		void set_speed(unsigned char s) {
			if (s > 15)
				return;

			this->speed = s;
		}

		bool tx(uint16_t cmd) {
			cout << "[CAR] Tx: " << hex << "{0x" << cmd << "} "
				<< dec << command_names[(cmd & 0xf0) >> 4]
				<< " Speed=" << (cmd & 0xf) << endl;

			txcmd.store(cmd);

			return true;
		}

		bool move(dir_mask_t dir) {
			uint16_t cmd = (speed | direction_to_command(dir));
			
			return tx(cmd);
		}

		/* this is so stupid */
		static move_cmd_t direction_to_command(dir_mask_t dir) {
			if (dir & DIRECTION_UP) {
				if (dir & DIRECTION_LEFT)
					return kLeftForward;
				else if (dir & DIRECTION_RIGHT)
					return kRightForward;

				return kForward;
			}
			else if (dir & DIRECTION_DOWN) {
				if (dir & DIRECTION_LEFT)
					return kLeftForward;
				else if (dir & DIRECTION_RIGHT)
					return kRightBackward;

				return kBackward;
			}
			else if (dir & DIRECTION_LEFT)
				return kLeft;
			else if (dir & DIRECTION_RIGHT)
				return kRight;

			return kStop;
		}
	};

	static int hci_control_socket = -1;

	struct ScanEntry {
		string name;
		string addr;

		int clock_offset;

		ScanEntry(string name, string addr, int clock_offset) : name(name), addr(addr), clock_offset(clock_offset) {}
	};

	/*
	 * code borrowed from hcitool
	 */
	struct BTDevice {
		int dev_id;

		BTDevice(int dev_id) : dev_id(dev_id) {

		}

		/*
		 * pairing is too complicated for me to implement myself so i
		 * took an existing implementation from the meego test suite.
		 */
		int pair(bdaddr_t addr, int clock_offset, uint8_t (&pin_code)[16]) {

			bt_ctx context = {0};

			context.remote_mac = addr;
			context.dev_id = dev_id;
			context.clock_offset = clock_offset;
			context.test_timeout.tv_sec = 10;
			context.hci_fd = hci_open_dev(dev_id);

			if (context.hci_fd < 0) {
				cout << "[BT:" << dev_id << "] HCI open failed for pairing: " << strerror(errno) << endl;
				return -1;
			}

			configure_adapter(context.hci_fd, dev_id);

			/* copy the PIN */
			memmove(&context.pin_code, &pin_code, 16);

			cout << "[BT:" << dev_id << "] Starting pairing thread ... " << endl;

			/*
			 * HCI service thread. this is like HCID except very limited. it has
			 * to stay running while the connection is active.
			 */
			thread([=]() mutable {
				cout << "[BT:" << dev_id << "] Pairing thread started ... " << endl;
				int r = wait_for_pairing(&context);
				cout << "[BT:" << dev_id << "] Pairing thread done with status: " << r << endl;
			}).detach();

			return 0;
		}

		int connect(ScanEntry& entry, string pin) {
			uint8_t pin_code[16] = {0};
			bdaddr_t ba;

			cout << "[BT:" << dev_id << "] Connecting to " << entry.addr << endl;

			/* ba */
			assert(!str2ba(entry.addr.c_str(), &ba));

			/* pin */
			strncpy(reinterpret_cast<char*>(&pin_code), pin.c_str(), sizeof(pin_code));

			int ret = pair(ba, entry.clock_offset, pin_code);

			return ret;
		}

		/* based on HCI stuff from BlueZ */
		vector<ScanEntry> scan() {
			uint8_t lap[3] = { 0x33, 0x8b, 0x9e };
			int num_rsp, length, flags;
			uint8_t cls[3], features[8];
			char addr[18], name[249], *comp;

			inquiry_info *info = nullptr;

			std::vector<ScanEntry> v;

			length  = 8;	/* ~10 seconds */
			num_rsp = 0;
			flags   = 0;

			num_rsp = hci_inquiry(dev_id, length, num_rsp, lap, &info, flags);
			if (num_rsp < 0) {
				cout << "[BT:" << dev_id << "] HCI inquiry failed: " << strerror(errno) << endl;
				return v;
			}

			cout << "[BT:" << dev_id << "] Scan found " << num_rsp << " devices" << endl;

			assert(info);

			int dd = hci_open_dev(dev_id);

			close_scope Z_dd(dd);

			for (int i = 0; i < num_rsp; i++) {
				uint16_t handle = 0;

				//cout << "[BT:" << dev_id << "]    ba2str ..." << name << endl;

				ba2str(&(info+i)->bdaddr, addr);

				cout << "[BT:" << dev_id << "]    reading remote" << addr  << " ..." << endl;

				if (hci_read_remote_name_with_clock_offset(dd,
						&(info+i)->bdaddr,
						(info+i)->pscan_rep_mode,
						(info+i)->clock_offset | 0x8000,
						sizeof(name), name, 5000) < 0)
					strcpy(name, "n/a");

				for (int n = 0; n < 248 && name[n]; n++) {
					if ((unsigned char) name[i] < 32 || name[i] == 127)
						name[i] = '.';
				}

				name[248] = '\0';
				cout << "[BT:" << dev_id << "]        {" << i << "} :  addr=" << addr << " " << (info+i)->clock_offset << " '" << name << "'" << endl;

				v.push_back( ScanEntry(name, addr, (info+i)->clock_offset | 0x8000) );

				continue;

			}

			hci_close_dev(dd);

			return v;
		}

		~BTDevice() {
		}
	};

	static unique_ptr<BTDevice> current_device;

	static int start_all_devices() {
		/* int hci_for_each_dev(int flag, int(*func)(int dd, int dev_id, long arg), long arg); */

		struct hci_dev_list_req *dl;
		struct hci_dev_req *dr;
		int i, err = 0;

		/* cache that as that will stay unless the driver is unloaded */
		if (hci_control_socket == -1) {
			int sk = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
			if (sk < 0) {
				cout << "[BT] Can't open HCI device!" << endl;
				return -1;
			}
			hci_control_socket = sk;
		}

		dl = reinterpret_cast<hci_dev_list_req*>(malloc(HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl)));
		if (!dl) {
			err = errno;
			cout << "[BT] Can't allocate memory!" << endl;
			return 0;
		}

		free_scope Z_dl(dl);

		memset(dl, 0, HCI_MAX_DEV * sizeof(*dr) + sizeof(*dl));

		dl->dev_num = HCI_MAX_DEV;
		dr = dl->dev_req;

		if (ioctl(hci_control_socket, HCIGETDEVLIST, (void *) dl) < 0) {
			err = errno;
			cout << "[BT] Can't get HCI device list!" << endl;
			return -1;
		}

		for (i = 0; i < dl->dev_num; i++, dr++) {
			cout << "[BT]     dev {" << i << "} " << "id=" << dr->dev_id << " opt=" << dr->dev_opt << endl;

			/* device is down, bring it up */
			if (!hci_test_bit(HCI_UP, &dr->dev_opt)) {
				if (ioctl(hci_control_socket, HCIDEVUP, dr->dev_id) < 0) {
					cout << "[BT]         failed to bring up" << endl;
				}
				else {
					cout << "[BT]         brought up!" << endl;
				}
			}
		}

		return (i);
	}

	static void wait_for_hci(int &_dev_id) {
		int fd = -1;

		while(1) {
			int dev_id = -1;

			dev_id = hci_get_route(NULL);

			if (dev_id < 0) {
				cout << "[BT] Still waiting for an BT HCI " << dev_id << " device (errno=" << errno << ") ..." << endl;
				sleep(2);
				continue;
			}

			cout << "[BT] Got HCI " << dev_id << endl;

			_dev_id = dev_id;
			break;
		}
	}

	static void reinitialize_modem() {
		int dev_id;

		cout << "[BT] Finding and starting bluetooth devices ..." << endl;

		while(start_all_devices() == 0) {
			cout << "[BT] No bluetooth devices found, will try again ..." << endl;
			sleep(2);
		}

		wait_for_hci(dev_id);

		current_device.reset( new BTDevice(dev_id) );
	}
}

struct LinuxFramebuffer {
	static void set_text_mode(bool enabled) {
		if (access("/dev/tty0", F_OK)) {
			return;
		}

		int fd = open("/dev/tty0", O_RDWR | O_SYNC);
		if (enabled)
		{
			if (ioctl(fd, KDSETMODE, (void*) KD_TEXT)) {
				perror("can't enable framebuffer TEXT mode");
			}
		}
		else {
			if (ioctl(fd, KDSETMODE, (void*) KD_GRAPHICS)) {
				perror("can't disable framebuffer TEXT mode");
			}
		}

		close(fd);
	}
};

void mount_fs(const char* mp, const char* fstype) {
	int res = mount(fstype, mp, fstype, MS_NOATIME, 0);
	if (res) {
		cout << "failed to mount " << fstype << ": " << strerror(errno) << endl;
	}
}

static bool g_HasAtLeastOneInput = false;

struct HIDDevice {
	int fd;
	int write_pipe;
	char device_name[256];
	bool retry_wait;

	thread evthread;

	enum Type {
		kKeyboard,
		kMouse
	};

	struct TouchEvent {
		int x;
		int y;
		int pressure;
		bool down;
	};

	bool has_syn = false;

	Type ty;
	TouchEvent out_ev;

	void event_loop() {
		while(1) {
			input_event ev;

			ssize_t v = read(fd, &ev, sizeof(input_event));

			if (v == sizeof(input_event)) {
#if DEBUG_HID_STUFF
				cout << "[HID] scan: " << ev.type << " " << ev.code << " " << ev.value << endl;
#endif

				if (ty == kMouse) {
					/* filter down to keyboard events */
					if (ev.type == EV_SYN) {
						cout << "[HID] Touch: X=" << out_ev.x << " Y=" << out_ev.y << " " << endl;

						if (write(write_pipe, &out_ev, sizeof(TouchEvent)) != sizeof(TouchEvent)) {
							cout << "[HID] Write to " << write_pipe << " failed!" << endl;
						}
						out_ev.down = false;
					}
					else if (ev.type == EV_ABS) {
						if (ev.code == 0) out_ev.x = ev.value;
						else if (ev.code == 1) out_ev.y = ev.value;
					}
				}
				else {
					/* filter down to keyboard events */
					if (ev.type != EV_KEY && ev.type != EV_ABS)
						continue;

					if (write(write_pipe, &ev, sizeof(input_event)) != sizeof(input_event)) {
						cout << "[HID] Write to " << write_pipe << " failed!" << endl;
					}
				}			
			}
			else {
				cout << "[HID] Read failed, closing device!" << endl;

				close(fd);

				if (retry_wait)
					reopen_device();
				else
					return;
			}
		}
	}

	HIDDevice(Type ty) : fd(-1), ty(ty), write_pipe(-1) {
	}

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define test_bit(bit, array)  ((array[LONG(bit)] >> OFF(bit)) & 1)
	;

	void reopen_device() {
		int hidfd = -1;

		cout << "[HID] Attempting to find a HID event source for " << ty << " ..." << endl;

		while (hidfd < 0)
		{
			Directory dev("/dev/input/");
			vector<string> files = dev.files();

			bool had_kbd = g_IgnoreWeirdAppleKeyboardIssue;

			for (string name : files)
			{
				if (name.find("event") == 0)
				{
					unsigned long  bitmask[NBITS(EV_MAX)];

					hidfd = open((dev.path+name).c_str(), O_RDONLY);
					if (hidfd < 0) {
						cout << "[HID]     Failed to open: " << strerror(errno) << endl;
						continue;
					}

					close_scope Z_hidfd(hidfd);

					if(ioctl(hidfd, EVIOCGNAME(sizeof(device_name)), device_name) < 0) {
						cout << "[HID]     Failed to query name: " << strerror(errno) << endl;
						continue;
					}
					
					if (ioctl(hidfd, EVIOCGBIT(0, sizeof(bitmask)), &bitmask) < 0) {
						cout << "[HID]     Failed to query device type: " << strerror(errno) << endl;
						continue;
					}

					cout << "[HID]     Found: " << device_name << endl;

					if (ty == kKeyboard) {
						if ( !((string(device_name).find("oard") != string::npos) || (string(device_name).find("360") != string::npos)) ) {
							cout << "[HID]          Not a key input device, skipping ..."  << endl;
							continue;
						}
						if (!had_kbd && (string(device_name).find("Apple") != string::npos)) {
							had_kbd = true;
							cout << "[HID]          Apple keyboard detected, will pick next event node instead ..."  << endl;
							continue;
						}
					}

					if (ty == kMouse && (string(device_name).find("ADS7846") != 0)) {
						cout << "[HID]          Not a pointer input device, skipping ..."  << endl;
						continue;
					}

					cout << "[HID]          Selected!"  << endl;

					g_HasAtLeastOneInput = true;

					/* don't let close_scope close it */
					Z_hidfd.ignore();
					/* exit the loop */
					break;
				}
			}

			if (retry_wait && hidfd == -1) {
				cout << "[HID] Still waiting for a HID event source for " << ty << " ..." << endl;
				sleep(2);
			}

			if (!retry_wait)
				break;
		}

		fd = hidfd;
	}

	bool start(int target_pipe, bool wait, bool retry = true) {
		retry_wait = retry;
		write_pipe = target_pipe; 

		if (wait || !retry_wait)
			reopen_device();

		if (!retry_wait && fd == -1)
			return false;

		thread([&](){
			if (fd == -1)
				reopen_device();

			event_loop();
		}).detach();

		return true;
	}

};


namespace UI {

	/* i can't believe it's not windows */

	LRESULT CALLBACK Wproc(HWND,UINT,WPARAM,LPARAM);
	extern "C" void* MwFindClassByName(LPCSTR lpClassName);

	struct Window {
		HWND hwnd;
		HWND orig_hwnd;

		Window() {}

		Window(HWND hwnd) : hwnd(hwnd) {

		}

		Window(const char* window_class, int x, int y, int w, int h, unsigned flags, unsigned id, const char* title = "") :
		Window(GetDesktopWindow(), window_class, x, y, w, h, flags, id, title)
		{

		}

		Window(Window& parent, const char* window_class, int x, int y, int w, int h, unsigned flags, unsigned id, const char* title = "") :
		Window(parent.hwnd, window_class, x, y, w, h, flags, id, title)
		{

		}

		Window(HWND parent, const char* window_class, int x, int y, int w, int h, unsigned flags, unsigned id, const char* title = "") {

			if (!parent) {
				cerr << "Window: invalid parent!" << endl;
				return;
			}

			LPVOID lp = NULL;

			/* see of wndproc is implemented by the subclass */
			if (MwFindClassByName(window_class) == NULL) {
				cout << "registering class " << window_class << "" << endl;
				Window::create_class(window_class, Window::wndproc_dispatch);

				lp = (LPVOID)this;
			}

			orig_hwnd = CreateWindowEx(0L,
				window_class,
				title,
				flags,
				x,
				y,
				w,
				h,
				parent,
				(HMENU)id,
				NULL,
				lp);

			if (!orig_hwnd) {
				cerr << "Window: window creation failed!" << endl;
				return;
			}

			hwnd = orig_hwnd;
		}

		static void create_class(const char* name, WNDPROC proc) {
			WNDCLASS wndclass;

			wndclass.style          = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW | CS_NOCLOSE;
			wndclass.lpfnWndProc    = reinterpret_cast<WNDPROC>(proc);
			wndclass.cbClsExtra     = 0;
			wndclass.cbWndExtra     = 0;
			wndclass.hInstance      = 0;
			wndclass.hIcon          = 0;
			wndclass.hCursor        = 0;
			wndclass.hbrBackground  = reinterpret_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
			wndclass.lpszMenuName   = NULL;
			wndclass.lpszClassName  = name;

			RegisterClass(&wndclass);
		}

		void show() {
			if (!hwnd)
				return;

			ShowWindow(hwnd, SW_SHOW);
		}

		void hide() {
			if (!hwnd)
				return;

			ShowWindow(hwnd, SW_HIDE);
		}

		void update() {
			if (!hwnd)
				return;

			UpdateWindow(hwnd);
		}

		virtual void key_event(unsigned key, bool down) {

		}

		virtual void char_event(char c) {

		}

		void set_focus() {
			if (!hwnd)
				return;

			SetFocus(hwnd);
		}

		virtual LRESULT wndproc(UINT iMsg, WPARAM wParam, LPARAM lParam) {
			//cout << __PRETTY_FUNCTION__ << ": iMsg=" << iMsg << endl;

			switch (iMsg) {
				case WM_PAINT:
					{
						PAINTSTRUCT ps;
						HDC hdc;

						hdc = BeginPaint(hwnd, &ps);
						paint(hdc, &ps);
						EndPaint(hwnd, &ps);
					}
					break;
				case WM_KEYDOWN:
					key_event(wParam, true);
					break;
				case WM_KEYUP:
					key_event(wParam, false);
					break;
				case WM_CHAR:
					char_event(wParam);
					break;
				case WM_DESTROY:
					PostQuitMessage(0);
					break;
				default:
#if 0
				cout << "[!msg]" << ": " << iMsg << endl;
#endif
					return DefWindowProc(hwnd,iMsg,wParam,lParam);
			}

			return 0;
		}

		virtual void paint(HDC hdc, PAINTSTRUCT* ps) {

		}

		static LRESULT wndproc_dispatch(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam) {
			if (iMsg == WM_CREATE) {
				LPCREATESTRUCT cs = (LPCREATESTRUCT)lParam;
				SetWindowLong(hwnd, GWL_USERDATA, (LONG)cs->lpCreateParams);

				return 0;
			}
			else {
				LONG this_ptr = GetWindowLong(hwnd, GWL_USERDATA);

				if (this_ptr) {
					Window* wnd = reinterpret_cast<Window*>(this_ptr);

					wnd->hwnd = hwnd;

					return wnd->wndproc(iMsg, wParam, lParam);
				}
			}

			return DefWindowProc(hwnd, iMsg, wParam, lParam);
		}

		int midx(int w) {
			RECT r;
			GetWindowRect(GetDesktopWindow(), &r);

			return (r.right / 2) - (w / 2);
		}

		int midy(int h) {
			RECT r;
			GetWindowRect(GetDesktopWindow(), &r);

			return (r.bottom / 2) - (h / 2);
		}

		void invalidate() {
			InvalidateRect(hwnd, NULL, TRUE);
		}

		void invalidate(int x, int y, int w, int h) {
			RECT rc = {x, y, x+w, y+h};

			if( hwnd->style & WS_CAPTION )
				rc.bottom += GetSystemMetrics(SM_CYCAPTION);

			if( (hwnd->style & (WS_BORDER | WS_DLGFRAME)) != 0 ) {
				rc.bottom += GetSystemMetrics(SM_CYFRAME);
				rc.right += GetSystemMetrics(SM_CXFRAME);
			}

			InvalidateRect(hwnd, &rc, TRUE);
		}

		void set_foreground() {
			SetForegroundWindow(hwnd);
		}
	};

	struct MainWindow;
	struct PairWindow;
	struct ProgressWindow;
	struct ErrorWindow;

	/*
	 * Application controller.
	 */
	struct Application {
		unique_ptr<MainWindow> main_window;
		unique_ptr<PairWindow> pair_window;
		unique_ptr<ProgressWindow> progress_window;

		unique_ptr<ErrorWindow> error_window;

		/* instance of the car we're controlling */
		unique_ptr<Bluetooth::Car> car;

		static Application* _shared;

		Application() {
			_shared = this;
		}

		void init_builtin_classes() {
			cout << __PRETTY_FUNCTION__ << ": Initialising MW classes" << endl;

			MwRegisterButtonControl(NULL);
			MwRegisterEditControl(NULL);
			MwRegisterListboxControl(NULL);
			MwRegisterProgressBarControl(NULL);
			MwRegisterStaticControl(NULL);
			MwRegisterComboboxControl(NULL);
			MwRegisterScrollbarControl(NULL);
		}

		int runloop() {
			MSG msg;

			cout << __PRETTY_FUNCTION__ << ": Starting event loop" << endl;
			while (GetMessage(&msg, NULL, 0, 0)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}

			return msg.wParam;
		}

		void connect(Bluetooth::ScanEntry& entry);

		void quit();

		static Application& shared() {
			return *_shared;
		}

		/* implementated below other stuff */
		void show_scan_dialog();
		void init_default_hid();
		void on_connection_failure();

		int main();

		void do_three_point_turn() {
			cout << __FUNCTION__ << ": Performing a three point turn ..." << endl;
			if (car) {
				for (ReplayCommand& cmd : g_TurnSequence) {
					if (!car->tx(cmd.cmd)) {
						on_connection_failure();
						break;
					}
					if (cmd.sleep)
						usleep(cmd.sleep);
				}
			}
		}
	};

	Application* Application::_shared = nullptr;

	struct PairWindow : public Window {
		unique_ptr<Window> device_list;
		vector<Bluetooth::ScanEntry> scan_results;

		PairWindow() :
		Window("pair_window", midx(300), midy(240), 300, 240, WS_OVERLAPPEDWINDOW, 1, "Bluetooth Pairing") {
			RECT client_rect;

			GetClientRect(hwnd, &client_rect);

			device_list.reset(new Window(*this,
				"LISTBOX",
				client_rect.left + 10,
				client_rect.top + 30,
				client_rect.right - 20,
				client_rect.bottom - 45,
				WS_VSCROLL | WS_CHILD | WS_VISIBLE | WS_BORDER,
				9,
				"OK"));
		}

		void activate() {
			show();
			set_foreground();
			device_list->set_focus();
		}

		virtual void char_event(char c) override {
			int idx = SendMessage(device_list->hwnd, LB_GETCURSEL, 0, 0);

			if (idx == LB_ERR) {

			}
			else {
				Application::shared().connect(scan_results[idx]);
			}
		}

		void add_scan_results(vector<Bluetooth::ScanEntry> v) {
			scan_results = v;

			SendMessage(device_list->hwnd, LB_RESETCONTENT, 0U, 0);

			for (Bluetooth::ScanEntry& e : scan_results) {
				string addstr = e.name + string(" (") + e.addr + string(")");

				/*
				 * XXX does MW copy it? i hope so.
				 */
				add_item(addstr.c_str());
			}

			if (v.size() != 0)
				SendMessage(device_list->hwnd, LB_SETCURSEL, 0U, 0);
		}

		void add_item(const char* name) {
			SendMessage(device_list->hwnd, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
		}

		virtual void paint(HDC hdc, PAINTSTRUCT* ps) override {
			RECT client_rect;

			GetClientRect(hwnd, &client_rect);

			RECT rect = {
				10,
				10,
				client_rect.left,
				client_rect.right
			};

			SetTextColor(hdc, RGB(0, 0, 0));
			DrawText(hdc, "Select a device and press ENTER to pair.", -1, &rect, DT_SINGLELINE);
		}
	};

	struct ProgressWindow : public Window {
		const char* text;

		ProgressWindow(const char* v) :
		Window("progress_window", midx(200), midy(50), 200, 50, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 1, "Please wait ...") {
			text = v;
		}

		virtual void paint(HDC hdc, PAINTSTRUCT* ps) override {
			RECT rect;

			GetClientRect(hwnd, &rect);
			DrawText(hdc, text, -1, &rect,
					 DT_SINGLELINE|DT_CENTER|DT_VCENTER);
		}

		void set_text(const char* text) {
			this->text = text;

			InvalidateRect(hwnd, NULL, TRUE);
			PostMessage(hwnd, WM_PAINT, 0, 0L);
			update();
		}

		void show_with_text(const char* text) {
			show();
			set_text(text);
			set_foreground();
		}
	};

	struct MainWindow : public Window {

#define DIR_BUTTON_SIZE 40

		int btn_coords[4][2];
		bool connected;
		string speed_str;

		dir_mask_t direction;

		MainWindow() :
		Window("main_window", midx(300), midy(200), 300, 200, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 1, "Bluetooth Car Control"),
		direction(0),
		connected(false)
		{
			init_dir_indicators();

			set_foreground();
		}

		/*
		 * ARGH THE BUTTONS SHOULD HAVE BEEN WINDOWS ...
		 *
		 * now the invalidation code is a nightmare but oh well, i don't
		 * feel like fixing it.
		 */

		void init_dir_indicators() {
			RECT client_rect;
			GetClientRect(hwnd, &client_rect);

			int sz = DIR_BUTTON_SIZE, spacing = 6;

			/* space for 3 buttons at the bottom */
			int y = client_rect.bottom - sz - spacing,
				x = (client_rect.right / 2) - ((sz + spacing) * 3 / 2);

			/* map out button positions */
			btn_coords[0][0] = x;
			btn_coords[0][1] = y;
			x += sz + spacing;
			btn_coords[1][0] = x;
			btn_coords[1][1] = y;
			x += sz + spacing;
			btn_coords[2][0] = x;
			btn_coords[2][1] = y;
			x -= sz + spacing;
			y -= sz + spacing;
			btn_coords[3][0] = x;
			btn_coords[3][1] = y;
		}

		void draw_dir_button(HDC hdc, int x, int y, int sz, dir_mask_t mask) {
			Draw3dOutset(hdc, x, y, sz, sz);

			if (direction & mask) {
				RECT rc = {x+2, y+2, x+sz-2, y+sz-2};

				/*
				 * this button is on, fill it with green.
				 */
				FastFillRect(hdc, &rc, RGB(200, 0, 0));
			}
		}

		inline void maskcheck(dir_mask_t mask, bool set, int coordset) {
			dir_mask_t before = direction;

			if (set)
				direction |= mask;
			else
				direction &= ~mask;

			if (before != direction) {
				invalidate(
					btn_coords[coordset][0]+2,
					btn_coords[coordset][1]+2,
					DIR_BUTTON_SIZE-2,
					DIR_BUTTON_SIZE-2
				);
			}
		}

		virtual void key_event(unsigned key, bool down) override {
			switch (key) {
				case VK_LEFT:
					maskcheck(DIRECTION_LEFT, down, 0);
					break;
				case VK_DOWN:
					maskcheck(DIRECTION_DOWN, down, 1);
					break;
				case VK_RIGHT:
					maskcheck(DIRECTION_RIGHT, down, 2);
					break;
				case VK_UP:
					maskcheck(DIRECTION_UP, down, 3);
					break;
				case VK_RETURN:
					if (down)
						Application::shared().show_scan_dialog();
					break;
				case 'q':
					cout << "Q!" << endl;
					break;
			}

			/* if we have a car instance, control it */
			if (Application::shared().car) {
				if (Application::shared().car->tx_thread_exited.load()) {
					Application::shared().on_connection_failure();
					return;
				}

				if (down) {
					unsigned char cs = Application::shared().car->speed;
					/* set_speed will check for overflow */
					if (key == 'u') {
						Application::shared().car->set_speed(--cs);
						invalidate();
					}
					else if (key == 'i') {
						Application::shared().car->set_speed(++cs);
						invalidate();
					}
					else if (key == 'z') {
						Application::shared().do_three_point_turn();
					}
				}

				bool ret = Application::shared().car->move(direction);

				if (!ret) {
					/* tx err, notify app controller */
					Application::shared().on_connection_failure();
				}
			}
		}

		void update_speed_str() {
			if (Application::shared().car) {
				stringstream s;

				s << "Speed: " <<  static_cast<int>(Application::shared().car->speed)
						<< " ('i' to increase, 'u' to decrease)";

				speed_str = s.str();
			}
		}

		virtual void paint(HDC hdc, PAINTSTRUCT* ps) override {
			RECT client_rect;

			GetClientRect(hwnd, &client_rect);

			/* status labels */
			{
				RECT rect = {
					10,
					5,
					client_rect.left,
					client_rect.right
				};

				int incr = 15;

				SelectObject(hdc, GetStockObject(SYSTEM_FIXED_FONT));
				if (connected) {
					SetTextColor(hdc, RGB(0, 0, 255));
					DrawText(hdc, "Status: CONNECTED", -1, &rect, DT_SINGLELINE);
				}
				else {
					SetTextColor(hdc, RGB(255, 0, 0));
					DrawText(hdc, "Status: DISCONNECTED", -1, &rect, DT_SINGLELINE);
				}

				rect.top += 10; rect.bottom += 10;

				SelectObject(hdc, GetStockObject(SYSTEM_FONT));
				SetTextColor(hdc, RGB(0, 0, 0));

				if (connected) {
					update_speed_str();

					rect.top += incr; rect.bottom += incr;
					DrawText(hdc, "Use arrow keys to move, 'z' for T.P.T.", -1, &rect, DT_SINGLELINE);
					rect.top += incr; rect.bottom += incr;
					DrawText(hdc, speed_str.c_str(), -1, &rect, DT_SINGLELINE);
				}

				rect.top += incr; rect.bottom += incr;
				DrawText(hdc, "Pair to a different device: ENTER", -1, &rect, DT_SINGLELINE);
				rect.top += incr; rect.bottom += incr;
				DrawText(hdc, "Quit and Shut Down: ESC", -1, &rect, DT_SINGLELINE);
			}
			/* directional indicators */
			{
				/* HDC hDC,int x,int y,int w,int h,COLORREF crTop,COLORREF crBottom */

				draw_dir_button(hdc, btn_coords[0][0], btn_coords[0][1], DIR_BUTTON_SIZE, DIRECTION_LEFT);
				draw_dir_button(hdc, btn_coords[1][0], btn_coords[1][1], DIR_BUTTON_SIZE, DIRECTION_DOWN);
				draw_dir_button(hdc, btn_coords[2][0], btn_coords[2][1], DIR_BUTTON_SIZE, DIRECTION_RIGHT);
				draw_dir_button(hdc, btn_coords[3][0], btn_coords[3][1], DIR_BUTTON_SIZE, DIRECTION_UP);
			}
		}
	};

	struct ErrorWindow : public Window {
		const char* text;

		ErrorWindow() :
		Window("error_window", midx(310), midy(100), 310, 100, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 1, "Error") {
			text = "ZZZ";
		}

		virtual void paint(HDC hdc, PAINTSTRUCT* ps) override {
			RECT rect;

			GetClientRect(hwnd, &rect);

			rect.top = 30;
			SetTextColor(hdc, RGB(0, 0, 0));
			DrawText(hdc, text, -1, &rect, DT_CENTER);

			rect.top = 60;
			DrawText(hdc, "Press ENTER to dismiss this message", -1, &rect, DT_CENTER);

			rect.top = 5;
			SetTextColor(hdc, RGB(255, 255, 255));
			SetBkColor(hdc, RGB(255, 0, 0));
			DrawText(hdc, "An error has occured:", -1, &rect, DT_CENTER);
		}

		virtual void key_event(unsigned key, bool down) override {
			cout << "ErrorWindow KEY EVENT!!!!!  " << key << endl;

			if (!down)
				return;

			switch (key) {
				case VK_RETURN:
					hide();
					Application::shared().main_window->set_focus();
			}
		}

		void set_text(const char* text) {
			this->text = text;

			InvalidateRect(hwnd, NULL, TRUE);
			PostMessage(hwnd, WM_PAINT, 0, 0L);
			update();
		}

		void show_with_text(const char* text) {
			show();
			set_text(text);
			set_foreground();
		}
	};

	extern "C" SCREENDEVICE scrdev;

	namespace IO {
		namespace Mouse {
			int pipe[2];

			static int Open(struct _mousedevice *) {
#if TOUCHSCREEN_ENABLED
				cout << __PRETTY_FUNCTION__ << "" << endl;

				::pipe(pipe);

				HIDDevice* mouse = new HIDDevice(HIDDevice::kMouse);
				
				if (mouse->start(pipe[1], false, false)) {
					/* hide the cursor */
					GdHideCursor(&scrdev);
					return pipe[0];
				}
				
				delete mouse;
#endif
				return -2;
			}

			static void Close(void) {

			}

			static int GetButtonInfo(void) {
				return 0;
			}

			static void GetDefaultAccel(int *pscale,int *pthresh) {
				*pscale = 3;
				*pthresh = 5;
			}

			static int Read(MWCOORD *dx,MWCOORD *dy,MWCOORD *dz,int *bp) {
				return 0;
			}

			static int Poll(void) {
				return 0;
			}
		}

		/*
		 * i feel like i should elaborate on this.
		 *
		 * the kbd data is written to a pipe on which MW then waits. done because:
		 *  1). MW is stupid and doesn't provide better mechanisms than polling the pipe
		 *  2). implementing something better is too much effort
		 *  3). polling the kdb evfd directly would make MW barf if the read fails (ie.
				keyboard is unplugged)
		 */
		namespace Kbd {
			int pipe[2];

			static unsigned keymap[] = {
				0 /* RESERVED */, MWKEY_ESCAPE /* ESC */, '1' /* 1 */,
				'2' /* 2 */, '3' /* 3 */, '4' /* 4 */,
				'5' /* 5 */, '6' /* 6 */, '7' /* 7 */,
				'8' /* 8 */, '9' /* 9 */, '0' /* 0 */,
				0 /* MINUS */, 0 /* EQUAL */, MWKEY_BACKSPACE /* BACKSPACE */,
				MWKEY_TAB /* TAB */, 'q' /* Q */, 'w' /* W */,
				'e' /* E */, 'r' /* R */, 't' /* T */,
				'y' /* Y */, 'u' /* U */, 'i' /* I */,
				'o' /* O */, 'p' /* P */, 0 /* LEFTBRACE */,
				0 /* RIGHTBRACE */, MWKEY_ENTER /* ENTER */, 0 /* LEFTCTRL */,
				'a' /* A */, 's' /* S */, 'd' /* D */,
				'f' /* F */, 'g' /* G */, 'h' /* H */,
				'j' /* J */, 'k' /* K */, 'l' /* L */,
				0 /* SEMICOLON */, 0 /* APOSTROPHE */, 0 /* GRAVE */,
				0 /* LEFTSHIFT */, 0 /* BACKSLASH */, 'z' /* Z */,
				'x' /* X */, 'c' /* C */, 'v' /* V */,
				'b' /* B */, 'n' /* N */, 'm' /* M */,
				0 /* COMMA */, 0 /* DOT */, 0 /* SLASH */,
				0 /* RIGHTSHIFT */, 0 /* KPASTERISK */, 0 /* LEFTALT */,
				' ' /* SPACE */, 0 /* CAPSLOCK */, 0 /* F1 */,
				0 /* F2 */, 0 /* F3 */, 0 /* F4 */,
				0 /* F5 */, 0 /* F6 */, 0 /* F7 */,
				0 /* F8 */, 0 /* F9 */, 0 /* F10 */,
				0 /* NUMLOCK */, 0 /* SCROLLLOCK */, 0 /* KP7 */,
				0 /* KP8 */, 0 /* KP9 */, 0 /* KPMINUS */,
				0 /* KP4 */, 0 /* KP5 */, 0 /* KP6 */,
				0 /* KPPLUS */, 0 /* KP1 */, 0 /* KP2 */,
				0 /* KP3 */, 0 /* KP0 */, 0 /* KPDOT */, 0 /* ??? */,
				0 /* ZENKAKUHANKAKU */, 0 /* 102ND */, 0 /* F11 */,
				0 /* F12 */, 0 /* RO */, 0 /* KATAKANA */,
				0 /* HIRAGANA */, 0 /* HENKAN */, 0 /* KATAKANAHIRAGANA */,
				0 /* MUHENKAN */, 0 /* KPJPCOMMA */, 0 /* KPENTER */,
				0 /* RIGHTCTRL */, 0 /* KPSLASH */, 0 /* SYSRQ */,
				0 /* RIGHTALT */, 0 /* LINEFEED */, 0 /* HOME */,
				MWKEY_UP /* UP */, 0 /* PAGEUP */, MWKEY_LEFT /* LEFT */,
				MWKEY_RIGHT /* RIGHT */, 0 /* END */, MWKEY_DOWN /* DOWN */,
				0 /* PAGEDOWN */, 0 /* INSERT */, 0 /* DELETE */,
				0 /* MACRO */, 0 /* MUTE */, 0 /* VOLUMEDOWN */,
				0 /* VOLUMEUP */, 0 /* POWER */, 0 /* KPEQUAL */,
				0 /* KPPLUSMINUS */, 0 /* PAUSE */
			};

			static int Open(KBDDEVICE *pkd) {
				cout << __PRETTY_FUNCTION__ << "" << endl;

				::pipe(pipe);

				return pipe[0];
			}

			static void Close(void) {
				cout << __PRETTY_FUNCTION__ << "" << endl;
			}

			static void GetModifierInfo(MWKEYMOD *modifiers, MWKEYMOD *curmodifiers) {
				cout << __PRETTY_FUNCTION__ << "" << endl;

				if (modifiers)
					*modifiers = 0;         /* no modifiers available */
				if (curmodifiers)
					*curmodifiers = 0;
			}

			static int abstokey(int val, int ty) {
				if (ty == ABS_Y) {
					if (val > 0)
						return KEY_DOWN;
					else if (val < 0)
						return KEY_UP;
					else
						return 0;
				}
				else if (ty == ABS_X) {
					if (val > 0)
						return KEY_RIGHT;
					else if (val < 0)
						return KEY_LEFT;
					else
						return 0;
				}
			}

			static void handle_abs(int& state, int ty, input_event* ev) {
				if (ev->value == 0 && state != 0) {
					/* write a keydown for old state */
					input_event synth_ev;

					synth_ev.code = abstokey(state, ty);
					synth_ev.value = 0;
					synth_ev.type = EV_KEY;

					write(pipe[1], &synth_ev, sizeof(input_event));
				}
				
				state = ev->value;

				/* fixup this event */
				ev->code = abstokey(ev->value, ty);
				ev->value = (ev->code != 0);
				ev->type = EV_KEY;

			}

			static void xpad_abs_to_normal(input_event* ev) {
				static int hatx_state, haty_state, x_state, y_state;

				/*
				 * dpad
				 */
				if (ev->code == ABS_HAT0X) {
					handle_abs(hatx_state, ABS_X, ev);
				}
				else if (ev->code == ABS_HAT0Y) {
					handle_abs(haty_state, ABS_Y, ev);
				}
				#if 0
				/*
				 * left stick.
				 */
				else if (ev->code == ABS_X) {
					handle_abs(x_state, ABS_X, ev);
				}
				else if (ev->code == ABS_Y) {
					handle_abs(y_state, ABS_Y, ev);
				}
				#endif			
			}

			static int Read(MWKEY *buf, MWKEYMOD *modifiers, MWSCANCODE *scancode) {
				//cout << __PRETTY_FUNCTION__ << "" << endl;

				input_event ev;
				ssize_t v = read(pipe[0], &ev, sizeof(input_event));

				MWKEY mwkey = 0;

				if (v == sizeof(input_event)) {
					if (ev.type == EV_ABS) {
						xpad_abs_to_normal(&ev);
					}

					int code = ev.code;

					if (code <= (sizeof(keymap) / sizeof(unsigned))) {
						mwkey = keymap[code];
					}
					else if (ev.type == EV_KEY) {
						if (ev.code == BTN_A)
							mwkey = MWKEY_ENTER;
						else if (ev.code == BTN_X)
							mwkey = MWKEY_ESCAPE;
						else if (ev.code == BTN_Y)
							mwkey = 'z';
						else if (ev.code == BTN_TL)
							mwkey = 'u';
						else if (ev.code == BTN_TR)
							mwkey = 'i';
					}

					if (mwkey != 0) {
						*buf = mwkey;
						*modifiers = 0;
						*scancode = 0;

						if (ev.value == 1) {
							/* down */
							if (code == KEY_ESC) {
								Application::shared().quit();
							}
							return 1;
						}
						else if (ev.value == 0) {
							return 2;
						}
					}
				}

				return 0;
			}

			static int Poll(void) {
				cout << __PRETTY_FUNCTION__ << "" << endl;
			}
		}

		void init() {
			cout << __PRETTY_FUNCTION__ << ": Setting up mouse hooks" << endl;
			mousedev.Open = Mouse::Open;
			mousedev.Close = Mouse::Close;
			mousedev.GetButtonInfo = Mouse::GetButtonInfo;
			mousedev.GetDefaultAccel = Mouse::GetDefaultAccel;
			mousedev.Read = Mouse::Read;
			mousedev.Poll = Mouse::Poll;

			cout << __PRETTY_FUNCTION__ << ": Setting up keyboard hooks" << endl;
			kbddev.Open = Kbd::Open;
			kbddev.Close = Kbd::Close;
			kbddev.GetModifierInfo = Kbd::GetModifierInfo;
			kbddev.Read = Kbd::Read;
			kbddev.Poll = Kbd::Poll;
		}
	}

	/*################################################################################
	  #####
	 #     #  ####  #    # ##### #####   ####  #      #      ###### #####
	 #       #    # ##   #   #   #    # #    # #      #      #      #    #
	 #       #    # # #  #   #   #    # #    # #      #      #####  #    #
	 #       #    # #  # #   #   #####  #    # #      #      #      #####
	 #     # #    # #   ##   #   #   #  #    # #      #      #      #   #
	  #####   ####  #    #   #   #    #  ####  ###### ###### ###### #    #
	################################################################################*/

	void Application::on_connection_failure() {
		car = nullptr;

		main_window->connected = false;
		main_window->invalidate();

		error_window->show_with_text("Connection to car interrupted, please reconnect!");
	}

	void Application::connect(Bluetooth::ScanEntry& entry) {
		thread t([&](){
			char buf[256];
			snprintf(buf, sizeof(buf), "Connecting to %s ...", entry.addr.c_str());

			pair_window->hide();
			progress_window->show_with_text(buf);

			/* request pairing and connection */
			int ret = Bluetooth::current_device->connect(entry, g_PIN);

			if (ret != 0) {
				/* error */
				progress_window->hide();
				error_window->show_with_text("Failed to pair to Bluetooth device!");
			}
			else {
				/* success - next step, open an RFCOMM connection and
				 * create a Car object tied to the RFCOMM socket.
				 */

				unique_ptr<Bluetooth::Car> c( new Bluetooth::Car() );

				ret = c->connect(entry.addr);
				progress_window->hide();

				if (ret) {
					if (ret == -EACCES)
						error_window->show_with_text("Failed to pair with device (wrong PIN?)");
					else
						error_window->show_with_text("Failed to connect to Bluetooth device!");
				}
				else {
					car = std::move(c);

					/* update stuff */
					main_window->connected = true;
					main_window->invalidate();

					main_window->set_foreground();
				}
			}
		});

		t.detach();
	}

	void Application::show_scan_dialog() {
		thread([&](){
			if (main_window->connected) {
				car = nullptr;

				main_window->connected = false;
				main_window->invalidate();	
			}

			progress_window->show_with_text("Waiting for Bluetooth device ...");

			Bluetooth::reinitialize_modem();

			if (!Bluetooth::current_device) {
				error_window->show_with_text("No Bluetooth modem is connected!");
				return;
			}

			progress_window->set_text("Scanning ...");
			auto results = Bluetooth::current_device->scan();

			progress_window->hide();

			if (results.size() == 0) {
				error_window->show_with_text("No devices found during Bluetooth scan!");
			}
			else {
				pair_window->add_scan_results(results);
				pair_window->activate();
			}
		}).detach();
	}

	int Application::main() {
		init_builtin_classes();

		progress_window.reset(new ProgressWindow("Waiting for an input device ..."));

		progress_window->show();
		progress_window->update();

		init_default_hid();

		/*
		 * initialise our UI dialogs.
		 */
		main_window.reset(new MainWindow());
		pair_window.reset(new PairWindow());
		error_window.reset(new ErrorWindow());

		error_window->hide();
		pair_window->hide();

		main_window->set_focus();

		//pair_window->show();
		//pair_window->update();

		return runloop();
	}

	void Application::quit() {
		main_window->hide();

#if 1
		progress_window->set_text("Restarting ...");
		progress_window->show();

		reboot(RB_AUTOBOOT);
#else
	reboot(RB_POWER_OFF);
#endif
		_exit(0);
	}

	void Application::init_default_hid() {
		HIDDevice* keyboard = new HIDDevice(HIDDevice::kKeyboard);

		/*
		 * we don't need to wait for it right now if we already have an
		 * input device. if we don't, then we must.
		 */
		keyboard->start(UI::IO::Kbd::pipe[1], !g_HasAtLeastOneInput);
	}

	extern "C"
	int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
					   PSTR szCmdLine, int iCmdShow)
	{
		return unique_ptr<Application>(new Application())->main();
	}
}

/*
 * stupid error handling stuff. fork and run the main thing in a child, if it
 * exits, restart it ASAP.
 */
namespace Supervisor {
	void setup_UART() {
		/*
		 * redirect stdout/stderr to serial if we have it because the framebuffer is
		 * busy showing microwindows on it.
		 */
		int fd = open("/dev/ttyAMA0", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}
	}

	void workloop() {

		while (1) {
			/*
			 * fork, wait, repeat.
			 */

			int child_pid;

			child_pid = fork();

			if (child_pid < 0) {
				cout << "[SVC] Fork failed!" << endl;
				_exit(0);
			}
			else if (child_pid == 0) {
				cout << "[SVC] I'm the child!" << endl;
			}
			else {
				int status;

				/* parent */
				wait4(child_pid, &status, 0, 0);

				cout << "[SVC] Detected child exit, restarting ..." << endl;
				continue;
			}

			break;
		}
	}

	void init() {
		setup_UART();

		cout << "[SVC] Started!" << endl;

		cout << "[SVC] Mounting filesystems ..." << endl;
		mount_fs("/sys", "sysfs");

		workloop();
	}

	/* parse config file */
	void load_configuration() {
		ini_t ini { "/OVERRIDE.INI", true };

		string fbdev { ini["output"]["fb"] };
		string blank { "" };

		if (fbdev == blank || access(fbdev.c_str(), F_OK)) {
			fbdev = "/dev/fb0";
		}
		if (ini["bluetooth"]["pin"] != blank) {
			g_PIN = ini["bluetooth"]["pin"];
		}
		if (ini["bluetooth"]["channel"] != blank) {
			g_Channel = atoi(ini["bluetooth"]["channel"].c_str());
		}
		if (ini["control"]["udelay"] != blank) {
			g_UDelay = atoi(ini["control"]["udelay"].c_str());
		}
		if (ini["control"]["apple_doesnt_suck"] != blank) {
			g_IgnoreWeirdAppleKeyboardIssue = atoi(ini["control"]["apple_doesnt_suck"].c_str());
		}
		cout << "fb  = '" << fbdev << "'" << endl;
		cout << "pin = " << g_PIN << endl;
		cout << "ch  = " << g_Channel << endl;
		cout << "ud  = " << g_UDelay << endl;
		cout << "trn = " << ini["control"]["turn_sequence"] << endl;
		cout << "apple_doesnt_suck = " << g_IgnoreWeirdAppleKeyboardIssue << endl;

		/* unmarshal the turn sequence */
		if (ini["control"]["turn_sequence"] != blank) {
			string del { "," };

			string& s = ini["control"]["turn_sequence"];

			size_t pos = 0;

			ReplayCommand cmd;
			bool tok = false;

			while ((pos = s.find(del)) != std::string::npos) {
				string ss = s.substr(0, pos);


				if (tok) {
					cmd.sleep = atoi(ss.c_str());
					g_TurnSequence.push_back(cmd);
				}
				else {
					cmd.cmd = strtol(ss.c_str(), NULL, 16);
				}
				tok = !tok;

				s.erase(0, pos + del.length());
			}
		}
		
#if 1
		/* test sequence */
		for (ReplayCommand& cmd : g_TurnSequence)
			cout << "Replay: cmd=" << cmd.cmd << " sleep=" << cmd.sleep << endl;
#endif

		/* copy the string so it's happy */
		setenv("FRAMEBUFFER", fbdev.c_str(), 1);
	}
}

extern "C" int
MwUserInit(int ac, char **av) {
	Supervisor::init();
	Supervisor::load_configuration();

	

	cout << "[MW] Initialising MW input ..." << endl;
	UI::IO::init();

	return 0;
}
