/****************************************************
	Virtual Collaborative Teams - The base program 
    The main module
	****************************************************/

#include <windows.h>
#include <math.h>
#include <time.h>
#include <gl\gl.h>
#include <gl\glu.h>
#include <iterator> 
#include <map>
#include <stdio.h>

#include "objects.h"
#include "graphics.h"
#include "net.h"
using namespace std;

// --- Funkcja do tworzenia konsoli debugowej ---
void CreateDebugConsole()
{
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);
	freopen_s(&f, "CONOUT$", "w", stderr);
}

FILE *f = fopen("wlog.txt", "w"); // plik do zapisu informacji testowych

#define TYPE_STATE 0
#define TYPE_COLLISION 1 // Typ komunikatu kolizji

MovableObject *my_car;               // obiekt przypisany do tej aplikacji
Environment env;

map<int, MovableObject*> other_cars;

float avg_cycle_time;                // sredni czas pomiedzy dwoma kolejnymi cyklami symulacji i wyswietlania
long time_of_cycle, number_of_cyc;   // zmienne pomocnicze potrzebne do obliczania avg_cycle_time
long time_start = clock();

//multicast_net *multi_reciv;          // wsk do obiektu zajmujacego sie odbiorem komunikatow
//multicast_net *multi_send;           //   -||-  wysylaniem komunikatow

unicast_net* uni_reciv;
unicast_net* uni_send;

bool is_server = false; // 

HANDLE threadReciv;                  // uchwyt w¹tku odbioru komunikatów
HWND main_window;                    // uchwyt do g³ównego okna programu 
CRITICAL_SECTION m_cs;               // do synchronizacji w¹tków

bool if_SHIFT_pressed = false;
bool if_ID_visible = true;           // czy rysowac nr ID przy ka¿dym obiekcie
bool if_mouse_control = false;       // sterowanie za pomoc¹ klawisza myszki
int mouse_cursor_x = 0, mouse_cursor_y = 0;     // po³o¿enie kursora myszy

extern ViewParams viewpar;           // ustawienia widoku zdefiniowane w grafice

long duration_of_day = 600;         // czas trwania dnia w [s]

struct Frame                                      // g³ówna struktura s³u¿¹ca do przesy³ania informacji
{	
	int iID;                                      // identyfikator obiektu, którego 
	int type;                                     // typ ramki: informacja o stateie, informacja o zamkniêciu, komunikat tekstowy, ... 
	ObjectState state;                            // po³o¿enie, prêdkoœæ: œrodka masy + k¹towe, ...

	long sending_time;                            // tzw. znacznik czasu potrzebny np. do obliczenia opóŸnienia
	int iID_receiver;                             // nr ID odbiorcy wiadomoœci, jeœli skierowana jest tylko do niego
};


// --- KOD SERWERA ---
struct ClientData {
	unsigned long ip;    // IP klienta (w formacie liczbowym)
	ObjectState state;   // Aktualny stan jego pojazdu
};
map<int, ClientData> registered_clients;

DWORD WINAPI ServerReceiveThreadFun(void* ptr)
{
	Frame frame;
	while (1)
	{
		unsigned long sender_ip;
		int bytes = uni_reciv->reciv((char*)&frame, &sender_ip, (unsigned short)sizeof(Frame));

		if (bytes > 0 && frame.type == TYPE_STATE)
		{
			if (is_server) {
				printf("[Serwer] Odebrano pozycje od Gracza %d. Przesylam dalej...\n", frame.iID);

			}

			// Rejestracja / Aktualizacja stanu gracza na serwerze
			registered_clients[frame.iID].ip = sender_ip;
			registered_clients[frame.iID].state = frame.state;

			// LOGIKA KOLIZJI (Zadanie 5)
			for (auto& other : registered_clients)
			{
				if (other.first == frame.iID) continue; // Nie sprawdzamy kolizji z samym sob¹

				float dist = (frame.state.vPos - other.second.state.vPos).length();
				float r1 = 4.5f; // r1 (po³owa length)
				float r2 = 4.5f;

				if (dist < (r1 + r2))
				{
					Vector3 wektor_do_auta = other.second.state.vPos - frame.state.vPos;
					if ((frame.state.vV ^ wektor_do_auta) > 0)
					{
						printf("[!] KOLIZJA: Gracz %d uderzyl w gracza %d!\n", frame.iID, other.first);

						// Odsy³amy mu zakaz ruchu! U¿ywamy IP, które odebraliœmy z reciv
						Frame f_col;
						f_col.iID = other.first;
						f_col.type = TYPE_COLLISION;
						f_col.iID_receiver = frame.iID;
						uni_send->send((char*)&f_col, sender_ip, (unsigned short)sizeof(Frame));
					}
				}
			}

			// ROZSY£ANIE RUCHU DO INNYCH (Architektura klient-serwer)
			for (auto& other : registered_clients)
			{
				if (other.first != frame.iID)
				{
					// U¿ywamy funkcji send z numerycznym adresem IP (unsigned long)
					uni_send->send((char*)&frame, other.second.ip, (unsigned short)sizeof(Frame));
				}
			}
		}



	}
	return 1;
}
// -------------------

//******************************************
// Funkcja obs³ugi w¹tku odbioru komunikatów 
// UWAGA!  Odbierane s¹ te¿ komunikaty z w³asnej aplikacji by porównaæ obraz ekstrapolowany do rzeczywistego.
DWORD WINAPI ReceiveThreadFun(void *ptr)
{
	unicast_net *pmt_net = (unicast_net*)ptr;  // wskaŸnik do obiektu klasy multicast_net
	Frame frame;

	while (1)
	{
		unsigned long sender_ip;
		int frame_size = pmt_net->reciv((char*)&frame, &sender_ip, (unsigned short)sizeof(Frame));
		ObjectState state = frame.state;

		if (frame.iID != my_car->iID && frame_size > 0 && is_server==false) {
			printf("[Klient %d] Widze ruch gracza %d!\n", my_car->iID, frame.iID);
		}

		//fprintf(f, "odebrano stan iID = %d, ID dla mojego obiektu = %d\n", frame.iID, my_car->iID);

		// Lock the Critical section
		EnterCriticalSection(&m_cs);               // wejœcie na œcie¿kê krytyczn¹ - by inne w¹tki (np. g³ówny) nie wspó³dzieli³ 
	                                               // tablicy other_cars
		
		// Odbieranie informacji o kolizji
		if (frame.type == 1)
		{
			if (frame.iID_receiver == my_car->iID)
			{
				// Zatrzymujemy nasze auto
				my_car->state.vV.x = 0;
				my_car->state.vV.z = 0;
			}
		}
		
		if (frame.iID != my_car->iID)          // jeœli to nie mój w³asny obiekt
		{
			
			if ((other_cars.size() == 0) || (other_cars[frame.iID] == NULL))        // nie ma jeszcze takiego obiektu w tablicy -> trzeba go
				// stworzyæ
			{
				MovableObject *ob = new MovableObject();
				ob->iID = frame.iID;
				other_cars[frame.iID] = ob;		
				//fprintf(f, "zarejestrowano %d obcy obiekt o ID = %d\n", iLiczbaCudzychOb - 1, CudzeObiekty[iLiczbaCudzychOb]->iID);
			}
			other_cars[frame.iID]->ChangeState(state);   // aktualizacja stateu obiektu obcego 	
			
		}	
		//Release the Critical section
		LeaveCriticalSection(&m_cs);               // wyjœcie ze œcie¿ki krytycznej
	}  // while(1)
	return 1;
}

// *****************************************************************
// ****    Wszystko co trzeba zrobiæ podczas uruchamiania aplikacji
// ****    poza grafik¹   
void InteractionInitialisation()
{
	DWORD dwThreadId;
	time_of_cycle = clock();

	// Tworzymy auto zawsze, ¿eby grafika nie wywali³a b³êdu braku obiektu 
	// (serwer po prostu stworzy sobie "ducha", którym nie bêdzie sterowa³)
	my_car = new MovableObject();

	if (is_server)
	{
		printf("--- START SERWERA ---\n");
		uni_reciv = new unicast_net(1002); // Serwer odbiera na porcie 1002
		uni_send = new unicast_net(1001);  // Serwer wysy³a na port 1001

		// Uruchamiamy specjalny w¹tek serwerowy
		threadReciv = CreateThread(NULL, 0, ServerReceiveThreadFun, NULL, 0, &dwThreadId);
	}
	else
	{
		printf("--- START KLIENTA ---\n");
		uni_reciv = new unicast_net(1001); // Klient odbiera na porcie 1001
		uni_send = new unicast_net(1002);  // Klient wysy³a na port 1002

		// Uruchamiamy standardowy w¹tek klienta
		threadReciv = CreateThread(NULL, 0, ReceiveThreadFun, (void*)uni_reciv, 0, &dwThreadId);
	}
	SetThreadPriority(threadReciv, THREAD_PRIORITY_HIGHEST);
}

// *****************************************************************
// ****    Wszystko co trzeba zrobiæ w ka¿dym cyklu dzia³ania 
// ****    aplikacji poza grafik¹ 
void VirtualWorldCycle()
{
	if (is_server)
	{
		// Serwer wszystkie obliczenia wykonuje w w¹tku ServerReceiveThreadFun
		Sleep(10);
	}
	else
	{
		number_of_cyc++;

		if (number_of_cyc % 50 == 0)
		{
			char text[256];
			long prev_time = time_of_cycle;
			time_of_cycle = clock();
			float fFps = (50 * CLOCKS_PER_SEC) / (float)(time_of_cycle - prev_time);
			if (fFps != 0) avg_cycle_time = 1.0 / fFps; else avg_cycle_time = 1;

			sprintf(text, "WZR-lab (KLIENT) (%0.0f fps  %0.2fms) ", fFps, 1000.0 / fFps);
			SetWindowText(main_window, text);
		}

		my_car->Simulation(avg_cycle_time);

		Frame frame;
		frame.state = my_car->State();
		frame.iID = my_car->iID;
		frame.type = TYPE_STATE;

		uni_send->send((char*)&frame, (char*)"127.0.0.1", (unsigned short)sizeof(Frame));
	}
}

// *****************************************************************
// ****    Wszystko co trzeba zrobiæ podczas zamykania aplikacji
// ****    poza grafik¹ 
void EndOfInteraction()
{
	fprintf(f, "Koniec interakcji\n");
	fclose(f);
}

//deklaracja funkcji obslugi okna
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

HDC g_context = NULL;        // uchwyt contextu graficznego



//funkcja Main - dla Windows
int WINAPI WinMain(HINSTANCE hInstance,
	HINSTANCE hPrevInstance,
	LPSTR     lpCmdLine,
	int       nCmdShow)
{
	
	CreateDebugConsole();

	//Initilize the critical section
	InitializeCriticalSection(&m_cs);

	MSG message;		  //innymi slowy "komunikat"
	WNDCLASS main_class; //klasa g³ównego okna aplikacji

	static char class_name[] = "Klasa_Podstawowa";

	//Definiujemy klase g³ównego okna aplikacji
	//Okreslamy tu wlasciwosci okna, szczegoly wygladu oraz
	//adres funkcji przetwarzajacej komunikaty
	main_class.style = CS_HREDRAW | CS_VREDRAW;
	main_class.lpfnWndProc = WndProc; //adres funkcji realizuj¹cej przetwarzanie meldunków 
	main_class.cbClsExtra = 0;
	main_class.cbWndExtra = 0;
	main_class.hInstance = hInstance; //identyfikator procesu przekazany przez MS Windows podczas uruchamiania programu
	main_class.hIcon = 0;
	main_class.hCursor = LoadCursor(0, IDC_ARROW);
	main_class.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
	main_class.lpszMenuName = "Menu";
	main_class.lpszClassName = class_name;

	//teraz rejestrujemy klasê okna g³ównego
	RegisterClass(&main_class);

	main_window = CreateWindow(class_name, "WZR-lab lato 2025/26 temat 1 - wersja h", WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		100, 50, 950, 650, NULL, NULL, hInstance, NULL);

	

	ShowWindow(main_window, nCmdShow);

	//odswiezamy zawartosc okna
	UpdateWindow(main_window);

	// pobranie komunikatu z kolejki jeœli funkcja PeekMessage zwraca wartoœæ inn¹ ni¿ FALSE,
	// w przeciwnym wypadku symulacja wirtualnego œwiata wraz z wizualizacj¹
	ZeroMemory(&message, sizeof(message));
	while (message.message != WM_QUIT)
	{
		if (PeekMessage(&message, NULL, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessage(&message);
		}
		else
		{
			VirtualWorldCycle();    // Cykl wirtualnego œwiata
			InvalidateRect(main_window, NULL, FALSE);
		}
	}

	return (int)message.wParam;
}

/********************************************************************
FUNKCJA OKNA realizujaca przetwarzanie meldunków kierowanych do okna aplikacji*/
LRESULT CALLBACK WndProc(HWND main_window, UINT message_code, WPARAM wParam, LPARAM lParam)
{

	switch (message_code)
	{
	case WM_CREATE:  //message wysy³any w momencie tworzenia okna
	{

		g_context = GetDC(main_window);

		srand((unsigned)time(NULL));
		int result = GraphicsInitialisation(g_context);
		if (result == 0)
		{
			printf("nie udalo sie otworzyc okna graficznego\n");
			//exit(1);
		}

		InteractionInitialisation();

		SetTimer(main_window, 1, 10, NULL);

		return 0;
	}


	case WM_PAINT:
	{
		PAINTSTRUCT paint;
		HDC context;
		context = BeginPaint(main_window, &paint);

		DrawScene();
		SwapBuffers(context);

		EndPaint(main_window, &paint);

		return 0;
	}

	case WM_TIMER:

		return 0;

	case WM_SIZE:
	{
		int cx = LOWORD(lParam);
		int cy = HIWORD(lParam);

		WindowResize(cx, cy);

		return 0;
	}

	case WM_DESTROY: //obowi¹zkowa obs³uga meldunku o zamkniêciu okna

		EndOfInteraction();
		EndOfGraphics();

		ReleaseDC(main_window, g_context);
		KillTimer(main_window, 1);

		//LPDWORD lpExitCode;
		DWORD ExitCode;
		GetExitCodeThread(threadReciv, &ExitCode);
		TerminateThread(threadReciv,ExitCode);
		//ExitThread(ExitCode);

		//Sleep(1000);

		other_cars.clear();
		

		PostQuitMessage(0);
		return 0;

	case WM_LBUTTONDOWN: //reakcja na lewy przycisk myszki
	{
		int x = LOWORD(lParam);
		int y = HIWORD(lParam);
		if (if_mouse_control)
			my_car->F = 50.0;        // si³a pchaj¹ca do przodu
		break;
	}
	case WM_RBUTTONDOWN: //reakcja na prawy przycisk myszki
	{
		int x = LOWORD(lParam);
		int y = HIWORD(lParam);
		if (if_mouse_control)
			my_car->F = -5.0;        // si³a pchaj¹ca do tylu
		break;
	}
	case WM_MBUTTONDOWN: //reakcja na œrodkowy przycisk myszki : uaktywnienie/dezaktywacja sterwania myszkowego
	{
		if_mouse_control = 1 - if_mouse_control;
		if (if_mouse_control) my_car->if_keep_steer_wheel = true;
		else my_car->if_keep_steer_wheel = false;

		mouse_cursor_x = LOWORD(lParam);
		mouse_cursor_y = HIWORD(lParam);
		break;
	}
	case WM_LBUTTONUP: //reakcja na puszczenie lewego przycisku myszki
	{
		if (if_mouse_control)
			my_car->F = 0.0;        // si³a pchaj¹ca do przodu
		break;
	}
	case WM_RBUTTONUP: //reakcja na puszczenie lewy przycisk myszki
	{
		if (if_mouse_control)
			my_car->F = 0.0;        // si³a pchaj¹ca do przodu
		break;
	}
	case WM_MOUSEMOVE:
	{
		int x = LOWORD(lParam);
		int y = HIWORD(lParam);
		if (if_mouse_control)
		{
			float wheel_angle = (float)(mouse_cursor_x - x) / 20;
			if (wheel_angle > 60) wheel_angle = 60;
			if (wheel_angle < -60) wheel_angle = -60;
			my_car->state.steering_angle = PI*wheel_angle / 180;
			//my_car->steer_wheel_speed = (float)(mouse_cursor_x - x) / 20;
		}
		break;
	}
	case WM_KEYDOWN:
	{

		switch (LOWORD(wParam))
		{
		case VK_SHIFT:
		{
			if_SHIFT_pressed = 1;
			break;
		}
		case VK_SPACE:
		{
			my_car->breaking_factor = 1.0;       // stopieñ hamowania (reszta zale¿y od si³y docisku i wsp. tarcia)
			break;                       // 1.0 to maksymalny stopieñ (np. zablokowanie kó³)
		}
		case VK_UP:
		{
			my_car->F = 100.0;        // si³a pchaj¹ca do przodu
			break;
		}
		case VK_DOWN:
		{
			my_car->F = -70.0;
			break;
		}
		case VK_LEFT:
		{
			if (my_car->steer_wheel_speed < 0){
				my_car->steer_wheel_speed = 0;
				my_car->if_keep_steer_wheel = true;
			}
			else{
				if (if_SHIFT_pressed) my_car->steer_wheel_speed = 0.5;
				else my_car->steer_wheel_speed = 0.25 / 8;
			}

			break;
		}
		case VK_RIGHT:
		{
			if (my_car->steer_wheel_speed > 0){
				my_car->steer_wheel_speed = 0;
				my_car->if_keep_steer_wheel = true;
			}
			else{
				if (if_SHIFT_pressed) my_car->steer_wheel_speed = -0.5;
				else my_car->steer_wheel_speed = -0.25 / 8;
			}
			break;
		}
		case 'I':   // wypisywanie nr ID
		{
			if_ID_visible = 1 - if_ID_visible;
			break;
		}
		case 'W':   // cam_distance widoku
		{
			//cam_pos = cam_pos - cam_direct*0.3;
			if (viewpar.cam_distance > 0.5) viewpar.cam_distance /= 1.2;
			else viewpar.cam_distance = 0;
			break;
		}
		case 'S':   // przybli¿enie widoku
		{
			//cam_pos = cam_pos + cam_direct*0.3; 
			if (viewpar.cam_distance > 0) viewpar.cam_distance *= 1.2;
			else viewpar.cam_distance = 0.5;
			break;
		}
		case 'Q':   // widok z góry
		{
			if (viewpar.tracking) break;
			viewpar.top_view = 1 - viewpar.top_view;
			if (viewpar.top_view)
			{
				viewpar.cam_pos_1 = viewpar.cam_pos; viewpar.cam_direct_1 = viewpar.cam_direct; viewpar.cam_vertical_1 = viewpar.cam_vertical;
				viewpar.cam_distance_1 = viewpar.cam_distance; viewpar.cam_angle_1 = viewpar.cam_angle;
				viewpar.cam_pos = viewpar.cam_pos_2; viewpar.cam_direct = viewpar.cam_direct_2; viewpar.cam_vertical = viewpar.cam_vertical_2;
				viewpar.cam_distance = viewpar.cam_distance_2; viewpar.cam_angle = viewpar.cam_angle_2;
			}
			else
			{
				viewpar.cam_pos_2 = viewpar.cam_pos; viewpar.cam_direct_2 = viewpar.cam_direct; viewpar.cam_vertical_2 = viewpar.cam_vertical;
				viewpar.cam_distance_2 = viewpar.cam_distance; viewpar.cam_angle_2 = viewpar.cam_angle;
				viewpar.cam_pos = viewpar.cam_pos_1; viewpar.cam_direct = viewpar.cam_direct_1; viewpar.cam_vertical = viewpar.cam_vertical_1;
				viewpar.cam_distance = viewpar.cam_distance_1; viewpar.cam_angle = viewpar.cam_angle_1;
			}
			break;
		}
		case 'E':   // obrót kamery ku górze (wzglêdem lokalnej osi z)
		{
			viewpar.cam_angle += PI * 5 / 180;
			break;
		}
		case 'D':   // obrót kamery ku do³owi (wzglêdem lokalnej osi z)
		{
			viewpar.cam_angle -= PI * 5 / 180;
			break;
		}
		case 'A':   // w³¹czanie, wy³¹czanie trybu œledzenia obiektu
		{
			viewpar.tracking = 1 - viewpar.tracking;
			if (viewpar.tracking)
			{
				viewpar.cam_distance = viewpar.cam_distance_3; viewpar.cam_angle = viewpar.cam_angle_3;
			}
			else
			{
				viewpar.cam_distance_3 = viewpar.cam_distance; viewpar.cam_angle_3 = viewpar.cam_angle;
				viewpar.top_view = 0;
				viewpar.cam_pos = viewpar.cam_pos_1; viewpar.cam_direct = viewpar.cam_direct_1; viewpar.cam_vertical = viewpar.cam_vertical_1;
				viewpar.cam_distance = viewpar.cam_distance_1; viewpar.cam_angle = viewpar.cam_angle_1;
			}
			break;
		}
		case 'Z':   // zoom - zmniejszenie k¹ta widzenia
		{
			viewpar.zoom /= 1.1;
			RECT rc;
			GetClientRect(main_window, &rc);
			WindowResize(rc.right - rc.left, rc.bottom - rc.top);
			break;
		}
		case 'X':   // zoom - zwiêkszenie k¹ta widzenia
		{
			viewpar.zoom *= 1.1;
			RECT rc;
			GetClientRect(main_window, &rc);
			WindowResize(rc.right - rc.left, rc.bottom - rc.top);
			break;
		}
		case VK_F1:  // wywolanie systemu pomocy
		{
			char lan[1024], lan_bie[1024];
			//GetSystemDirectory(lan_sys,1024);
			GetCurrentDirectory(1024, lan_bie);
			strcpy(lan, "C:\\Program Files\\Internet Explorer\\iexplore ");
			strcat(lan, lan_bie);
			strcat(lan, "\\pomoc.htm");
			int wyni = WinExec(lan, SW_NORMAL);
			if (wyni < 32)  // proba uruchominia pomocy nie powiodla sie
			{
				strcpy(lan, "C:\\Program Files\\Mozilla Firefox\\firefox ");
				strcat(lan, lan_bie);
				strcat(lan, "\\pomoc.htm");
				wyni = WinExec(lan, SW_NORMAL);
				if (wyni < 32)
				{
					char lan_win[1024];
					GetWindowsDirectory(lan_win, 1024);
					strcat(lan_win, "\\notepad pomoc.txt ");
					wyni = WinExec(lan_win, SW_NORMAL);
				}
			}
			break;
		}
		case VK_ESCAPE:
		{
			SendMessage(main_window, WM_DESTROY, 0, 0);
			break;
		}
		} // switch po klawiszach

		break;
	}
	case WM_KEYUP:
	{
		switch (LOWORD(wParam))
		{
		case VK_SHIFT:
		{
			if_SHIFT_pressed = 0;
			break;
		}
		case VK_SPACE:
		{
			my_car->breaking_factor = 0.0;
			break;
		}
		case VK_UP:
		{
			my_car->F = 0.0;
			break;
		}
		case VK_DOWN:
		{
			my_car->F = 0.0;
			break;
		}
		case VK_LEFT:
		{
			my_car->Fb = 0.00;
			//my_car->state.steering_angle = 0;
			if (my_car->if_keep_steer_wheel) my_car->steer_wheel_speed = -0.25/8;
			else my_car->steer_wheel_speed = 0; 
			my_car->if_keep_steer_wheel = false;
			break;
		}
		case VK_RIGHT:
		{
			my_car->Fb = 0.00;
			//my_car->state.steering_angle = 0;
			if (my_car->if_keep_steer_wheel) my_car->steer_wheel_speed = 0.25 / 8;
			else my_car->steer_wheel_speed = 0;
			my_car->if_keep_steer_wheel = false;
			break;
		}

		}

		break;
	}

	default: //statedardowa obs³uga pozosta³ych meldunków
		return DefWindowProc(main_window, message_code, wParam, lParam);
	}


}

