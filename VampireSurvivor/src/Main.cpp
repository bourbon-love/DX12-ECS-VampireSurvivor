#include "game/GameApp.h"
#include "Win32Application.h"

_Use_decl_annotations_
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    VS::GameApp app(1280, 720, L"VampireSurvivor — DX12 + EnTT ECS");
    return Win32Application::Run(&app, hInstance, nCmdShow);
}
