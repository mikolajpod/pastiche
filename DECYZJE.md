# ML-Styler - dziennik decyzji projektowych

Data rozpoczęcia: 2026-09-12

## Ustalenia o środowisku

- Laptop: Quadro T2000 4 GiB (Turing TU117GLM, PCI 10DE:1FB8, compute 7.5,
  **bez rdzeni tensor**, za to dedykowane jednostki FP16 o podwójnej
  przepustowości względem FP32), driver 580.92, i9-9980HK, 64 GB RAM.
  Docelowo też RTX 3060 12 GiB (GA106, rdzenie tensor obecne).
  Uwaga: pierwotny zapis mówił o rdzeniach tensor w T2000 - to był błąd,
  sprostowany w D20. TU117 to wariant Turinga pozbawiony rdzeni RT i tensor.
- Toolchain jak w fractal-xplorer: MSYS2 MinGW GCC 15.2, CMake 4.2, Ninja.
  Brak MSVC, brak CUDA Toolkit, brak Qt. To ma pozostać (bez MSVC).

## D1. Zestaw algorytmów i silnik inferencji (2026-09-12)

- Aplikacja C++ robi wyłącznie inferencję. Trening stylów odbywa się offline
  w Pythonie (PyTorch), wynik eksportowany do ONNX.
- Silnik dla sieci feed-forward: ONNX Runtime (MIT, C API, linkuje się z MinGW).
- Silnik dla dyfuzji: stable-diffusion.cpp / ggml (MIT).
- Algorytmy w 1.0:
  - tryb szybki, dowolny styl: AdaIN (MIT) lub Magenta Arbitrary Image
    Stylization (Apache 2.0);
  - tryb szybki, styl wytrenowany (Johnson): 4 modele BSD-3 z pytorch/examples
    w paczce; modele jcjohnsona (licencja "personal/research only") tylko jako
    opcjonalny download przez użytkownika z wyświetleniem licencji, nigdy
    w repozytorium ani w wydaniu; własne style przez skrypt treningowy
    (ok. 1.5 h na T2000 dla sensownego wyniku, 5-7 h pełny trening);
  - tryb wolny, wysoka jakość: metoda dyfuzyjna (StyleID lub pochodna) na
    stable-diffusion.cpp; na 4 GB z kwantyzacją, na 3060 pełna.
- Gatys (metoda optymalizacyjna) nie wchodzi do 1.0. Uzasadnienie: wymaga
  autogradu, czyli LibTorch i MSVC; Johnson/Ulyanov są wizualnie prawie
  nieodróżnialne od Gatysa (Johnson 2016, Jing 2020); dyfuzja daje wyższą
  jakość. Jeśli kiedyś wróci, to jako NNST-Opt lub STROTSS, nie oryginał.
  Materiały porównawcze: research/cmp_*.png, sekcja w notatce.

## D2. Backend GPU (2026-09-12) - wariant C

- Feed-forward: ONNX Runtime z DirectML Execution Provider na Windows
  (jedna DLL ok. 40 MB, działa na NVIDIA, Intel, AMD). CUDA Execution
  Provider jako opcjonalny pakiet runtime (ok. 1.5 GB: cudart, cuBLAS,
  cuDNN 9) pobierany osobno, wybierany flagą / w ustawieniach; domyślny
  backend na Linuxie. CPU EP jako ostateczny fallback.
- Dyfuzja: stable-diffusion.cpp z backendem Vulkan (buduje się MinGW,
  bez nvcc i MSVC). Na NVIDIA ok. 70-90 % wydajności CUDA.
- Odrzucone: CUDA wszędzie (sd.cpp z CUDA wymaga nvcc + MSVC), Vulkan
  wszędzie (ORT nie ma dojrzałego Vulkan EP).
- Uwaga: qt6-base jest zainstalowane w MSYS2 (6.10.1), SDL2, Vulkan
  headers/loader, libpng, libjpeg-turbo, libwebp, libjxl, libtiff również.
  ONNX Runtime nie ma w MSYS2 - binaria z NuGet Microsoftu.

## D3. GUI: Dear ImGui + SDL2 + OpenGL 3.3 (2026-09-12)

- Jak w fractal-xplorer. MIT, ok. 2 MB, znany pipeline pakowania.
- Wymóg GPU dla GUI nie jest problemem, bo aplikacja i tak liczy na GPU.
- Dialogi plików: nativefiledialog-extended (zlib) lub równoważna.
- Plik wejścia GUI: guimain.cpp (zamiast qtmain.cpp). CLI: main.cpp.
- Qt odrzucone: LGPL v3 i 30-40 MB DLL-ek bez korzyści dla siatki 2x2
  z pokrętłami.

## D4. Interfejs algorytmu i parametry (2026-09-12)

- Każdy algorytm: jeden plik cpp, klasa implementująca IStyleAlgorithm
  (id, params(), estimateVram(), run()), rejestrowana makrem
  REGISTER_ALGORITHM w statycznym rejestrze.
- Parametry deklarowane jako dane (ParamSpec: klucz, typ, zakres, wartość
  domyślna, opis). GUI rysuje widgety z ParamSpec, CLI parsuje i waliduje
  `-p klucz=wartość` z ParamSpec, `--help` generowane z tego samego.
  Dodanie algorytmu nie wymaga zmian w main.cpp ani guimain.cpp.
- Składnia CLI: `ml-styler <in> <style> <out> <algo> [-p k=v ...]`.
- Obok każdego wyniku YYYYMMDD_HHMMSS.png zapisywany YYYYMMDD_HHMMSS.json
  z algorytmem, parametrami, ścieżkami wejść i wersją aplikacji.

## D5. Brak VRAM, skalowanie, kafelkowanie (2026-09-12)

- Przed załadowaniem modelu: algorytm liczy estymację VRAM ze wzoru na
  aktywacje (estimateVram), aplikacja odczytuje wolną pamięć GPU (DXGI na
  Windows, Vulkan na Linux). Realnie wolne na T2000: ok. 3.3 GB z 4 GB.
- Gdy estymacja > wolne: ODMOWA w CLI i GUI, z komunikatem podającym
  potrzebną i dostępną ilość oraz konkretną sugerowaną rozdzielczość
  ("przeskaluj do 1600 px") i ewentualnie kafelkowanie. Nic nie dzieje się
  automatycznie za plecami użytkownika.
- Skalowanie wbudowane: parametr wspólny `size=N` (dłuższy bok) w CLI,
  pole + przycisk "zastosuj sugestię" w GUI.
- Kafelkowanie: opcja jawna `tile=on`, nigdy domyślnie, tylko feed-forward.
  Ryzyko: Instance Normalization liczy statystyki per kafelek, możliwe
  przesunięcia tonu między kafelkami na gładkich obszarach. AdaIN od 1.0
  z globalnymi statystykami (enkoder i dekoder jako osobne ONNX, operacja
  AdaIN w C++). Johnson w wersji prostej z zakładką 128 px i miękkim
  przejściem, z ostrzeżeniem w opisie parametru. Dyfuzja: bez kafelkowania,
  tylko skalowanie w dół.
- Orientacyjne limity na T2000: Johnson/AdaIN FP16 do ok. 2000 px, FP32 do
  ok. 1400 px; SD 1.5 skwantyzowane 512-768 px; SDXL nie mieści się.

## D6. Formaty plików (2026-09-12)

- Wejście wymagane: JPEG (libjpeg-turbo), PNG (libpng), WebP (libwebp).
  "webm" w pierwotnym opisie oznaczało WebP; wideo poza zakresem.
- Wejście opcjonalne: JPEG XL (libjxl, flaga HAVE_JXL jak w fractal-xplorer).
- Wyjście wymagane: PNG. Opcjonalne: JPEG XL bezstratny.
- Orientacja EXIF w JPEG jest stosowana przy wczytywaniu (własny mały
  parser EXIF, bez libexif).
- Wewnętrznie 8 bit sRGB, profil ICC ignorowany.
- Odrzucone: stb_image (i tak potrzebne libwebp i libpng do zapisu).

## D7. Metoda dyfuzyjna (2026-09-12)

- 1.0: IP-Adapter + img2img na SD 1.5 przez stockowy stable-diffusion.cpp
  (obraz stylu jako image prompt, obraz treści jako start img2img).
  Parametry: steps, strength, ip_scale, seed, prompt (opcjonalny).
  Na 4 GB: SD 1.5 w Q8 + VAE tiling, 512-768 px. Na 3060: IP-Adapter Plus
  na SDXL, 1024 px.
- Opcja: `controlnet=canny|depth` (ControlNet 1.1, Apache 2.0, +700 MB FP16).
- Odrzucone: StyleID (wymaga forka sd.cpp i modyfikacji grafu attention).
- Licencje wag: IP-Adapter i ControlNet Apache 2.0. SD 1.5 / SDXL to
  CreativeML OpenRAIL-M - pobierane przez użytkownika z wyświetleniem
  licencji, nigdy w repozytorium ani w wydaniu.
- sd.cpp potwierdzone: SD1.x/SDXL/FLUX, img2img, ControlNet SD1.5,
  IP-Adapter SD1.5+SDXL (w tym Plus), GGUF, Vulkan, VAE tiling, MIT.

## D8. Modele: lokalizacja, pobieranie, konwersja (2026-09-12)

- Katalog `models/` obok exe (przenośny ZIP jak fractal-xplorer), nadpisanie
  przez `--models-dir` lub zmienną środowiskową.
- W wydaniu tylko modele o licencji permisywnej: Johnson 4 style BSD-3
  (4 x 6.5 MB), AdaIN MIT (ok. 90 MB). Reszta przez pobieranie.
- Podkomenda `ml-styler download <nazwa>` i przycisk w GUI. Lista w
  `models.json` w repozytorium: URL, SHA-256, licencja. Weryfikacja sumy
  po pobraniu. Modele z licencją nietypową (jcjohnson, SD 1.5 OpenRAIL-M)
  wymagają wyświetlenia skrótu licencji i potwierdzenia.
- Źródła: GitHub Releases projektu dla małych plików, Hugging Face dla dużych.
- Konwersja PyTorch -> ONNX (Johnson, AdaIN) robiona raz skryptami Python
  w repozytorium (`tools/`), gotowe ONNX publikowane w Releases. Użytkownik
  końcowy nie potrzebuje Pythona. sd.cpp czyta safetensors/GGUF wprost.
- Rozmiary: SD 1.5 Q8 ok. 2 GB, IP-Adapter + CLIP ok. 1 GB, ControlNet FP16
  700 MB, CUDA EP runtime 1.5 GB (osobny pakiet).

## D9. Zachowanie GUI podczas liczenia (2026-09-12)

- Siatka 2x2: treść, styl, wynik, panel parametrów (generowany z ParamSpec).
- Jeden wątek roboczy; pętla ImGui odpytuje co klatkę strukturę stanu pod
  mutexem: postęp 0-1, tekst etapu, flaga zakończenia. Bez kolejek zdarzeń.
- Anulowanie: flaga sprawdzana między krokami dyfuzji / kafelkami
  (ORT RunOptions::SetTerminate, callback postępu sd.cpp). Przerwanie nie
  zapisuje pliku.
- Podgląd pośredni co N kroków tylko dla dyfuzji, w kafelku wyniku.
- W trakcie liczenia panel parametrów wyszarzony; podmiana obrazów wejścia
  dozwolona, dotyczy następnego uruchomienia.
- Zapis automatyczny do `<out-dir>/YYYYMMDD_HHMMSS.png` + `.json`,
  out-dir ustawiany w GUI, domyślnie `out/` obok exe. Dodatkowo "Zapisz jako"
  (PNG lub JXL) i "Użyj jako treść" (łańcuchowanie).

## D10. Linux i pakowanie (2026-09-12)

- Windows: przenośny ZIP jak fractal-xplorer (package.sh: ldd po DLL-kach
  MinGW, ZIP przez Pythona). W ZIP: exe, DLL MinGW, onnxruntime.dll +
  DirectML.dll z NuGet, małe modele w models/, README, LICENSE z sekcją
  licencji zależności. CUDA EP runtime i duże modele osobno przez download.
- Wybór backendu ORT przy starcie: pierwszy dostępny z listy CUDA, DirectML,
  CPU; komunikat, który działa; flaga `--backend` do wymuszenia.
- Linux: "teoretycznie": kompiluje się i działa, sprawdzane okazjonalnie,
  bez paczek (AppImage/Flatpak) w 1.0. Plik BUILD-linux.md z pakietami
  Debian/Ubuntu i Arch. Backend: CUDA EP lub CPU EP.
- Zależności: find_package dla bibliotek obecnych w MSYS2 i dystrybucjach
  (SDL2, libpng, libjpeg-turbo, libwebp, libjxl, Vulkan); FetchContent
  z przypiętym tagiem dla ImGui, nativefiledialog-extended,
  stable-diffusion.cpp; ONNX Runtime zawsze z binariów Microsoftu.
- Windows 7: skreślone (DirectML wymaga Win10, MSYS2 nie wspiera Win7).

## D11. Licencja i nazwa (2026-09-12)

- Licencja: MIT (jak fractal-xplorer). Plik THIRD-PARTY-LICENSES.md
  z pełnymi tekstami licencji zależności, kopiowany do ZIP-a. Skrypty
  Python w tools/ na tej samej licencji.
- Nazwa: Pastiche (termin na dzieło naśladujące styl innego artysty;
  Johnson 2016 używa go na wynik stylizacji). Binarka `pastiche`.
  Kolizje na GitHubie (2 apki Android, CLI dstein64/pastiche, pystiche)
  są kosmetyczne, brak produktu ani znaku towarowego. Repozytorium
  np. `pastiche-app`, żeby nie mylić z pystiche.
- Przegląd konkurencji: brak otwartej, natywnej, offline, wieloalgorytmowej
  apki z dowolnym obrazem stylu i dyfuzją. Istnieją: zamknięty "Style"
  (macdaddy.io, 20 presetów), zabawki Python/PyQt/Streamlit, apki Android,
  przykład OpenCV dnn, GUI do sd.cpp bez transferu stylu z obrazu.

## D12. Repozytorium, układ, testy (2026-09-12)

- VCS: Git (jak fractal-xplorer), repozytorium w D:\hg\style, docelowo
  GitHub. Mimo globalnej konwencji hg dla katalogu D:\hg.
- Układ: CMakeLists.txt, LICENSE, THIRD-PARTY-LICENSES.md, README.md,
  DECYZJE.md, BUILD-linux.md, package.sh, models.json;
  src/main.cpp (CLI), src/guimain.cpp (ImGui), src/core/ (Image, io,
  EXIF, ParamSpec, rejestr, VRAM probe), src/backends/ (OrtSession,
  SdCppSession), src/algos/ (johnson.cpp, adain.cpp, magenta.cpp?,
  sd_ipadapter.cpp); tools/ (Python: konwersja ONNX, trening, models.json);
  models/ (tylko README + ignore); research/; tests/.
- Testy: `--selftest` (każdy algorytm na 256 px: rozmiar, nie czarny,
  brak NaN, suma kontrolna vs referencja CPU EP z tolerancją),
  `--benchmark` (czasy per algorytm i rozdzielczość), doctest (MIT)
  dla parsera parametrów, EXIF, kafelkowania, estymacji VRAM.
- Brak testów GUI. CLI ma wystarczyć do autonomicznej pracy i weryfikacji
  aż do pełnego działania; GUI sprawdza użytkownik ręcznie.

## D13. Binarki i etapy (2026-09-12)

- Dwa pliki wykonywalne: `pastiche.exe` (main.cpp, konsolowe) i
  `pastiche-gui.exe` (guimain.cpp, okienkowe), oba linkowane ze statyczną
  biblioteką `pastiche-core`.
- Etapy (kolejność i podział to decyzja wykonawcy, każdy etap kończy się
  czymś weryfikowalnym z CLI):
  1. Szkielet: CMake, core (io JPEG/PNG/WebP, EXIF, zapis PNG, ParamSpec,
     rejestr), CLI z --help/--list-algos, algorytm "identity", selftest,
     doctest, package.sh.
  2. Johnson na ORT: konwersja 4 modeli BSD do ONNX, OrtSession
     (DirectML/CPU), estymacja VRAM, odmowa z sugestią, parametr size.
  3. AdaIN: enkoder/dekoder osobno, AdaIN w C++, kafelkowanie z globalnymi
     statystykami, alpha.
  4. GUI: ImGui, siatka 2x2, panel z ParamSpec, wątek roboczy, anulowanie,
     autozapis, download modeli.
  5. Dyfuzja: sd.cpp (FetchContent, Vulkan), IP-Adapter + img2img SD 1.5,
     podgląd pośredni, models.json z licencjami; test T2000 512 px, potem 3060.
  6. Wykończenie: ControlNet opcja, JXL, CUDA EP pakiet, BUILD-linux.md
     i kompilacja na Linuxie, THIRD-PARTY-LICENSES, README, wydanie 1.0.
- Magenta arbitrary stylization tylko jeśli AdaIN po etapie 3 okaże się
  wizualnie za słaby.

## D18. GUI: szczegóły realizacji (2026-09-13)

- Układ 2x2 w jednym oknie ImGui bez dokowania: treść, styl, wynik, panel
  parametrów. Każdy kafelek to child window z ramką; obraz skalowany do
  kafelka z zachowaniem proporcji, wyśrodkowany.
- Panel parametrów generowany w całości z ParamSpec (Int/Float -> slider gdy
  zakres domknięty, inaczej InputInt/InputFloat; Bool -> checkbox; Enum ->
  combo; String -> InputText). Opis parametru jako tooltip. Dodanie
  algorytmu nie wymaga zmian w guimain.cpp.
- Wątek roboczy: kopia obrazów i parametrów przekazywana do wątku przez
  shared_ptr/wartość, wynik odbierany co klatkę pod mutexem (D9). W trakcie
  liczenia panel wyszarzony (BeginDisabled), podmiana obrazów wejściowych
  dozwolona.
- Anulowanie: pojedyncze Run() w ORT trwa sekundy i nie da się go odpytywać
  od środka, więc OrtModel::run() uruchamia wątek-strażnik, który co 50 ms
  sprawdza Progress::cancelled() i ustawia flagę RunOptionsSetTerminate.
  run() zwraca wtedy sentinel kCancelled, algorytm zamienia go na
  RunResult::aborted(), plik nie powstaje. Flaga jest czyszczona
  (RunOptionsUnsetTerminate) przed każdym biegiem.
- Dialogi plików: nativefiledialog-extended v1.2.1 (zlib) przez FetchContent.
  Przeciąganie plików na okno: pierwszy plik jako treść, drugi jako styl.
- Autozapis do <out-dir>/YYYYMMDD_HHMMSS.png + .json; przy kolizji nazw
  dopisywany sufiks _2. Sidecar JSON wspólny dla CLI i GUI
  (src/core/sidecar.cpp).
- Weryfikacja GUI bez człowieka przy klawiaturze: tools/gui_shot.ps1
  uruchamia okno, odgrywa kliknięcia/wpisywanie i zapisuje zrzuty. To nie są
  testy automatyczne (D12 ich nie przewiduje), tylko narzędzie do obejrzenia
  skutków zmiany. Sprawdzone: wczytanie plików z argv, obrót EXIF, bieg
  Johnson/candy na DirectML (1,0 s), wynik w kafelku, autozapis z sidecarem,
  przełączenie backendu na CPU, anulowanie w trakcie inferencji (panel wraca,
  status "Cancelled.", brak pliku).

## D19. Pobieranie modeli przesunięte za etap 4 (2026-09-13)

- D13 wymieniał "download modeli" w etapie 4, ale nie ma dziś czego pobierać:
  wszystkie modele etapów 2-3 (Johnson, AdaIN) są w wydaniu, a repozytorium
  nie jest jeszcze na GitHubie, więc models.json nie miałby prawdziwych URL-i
  ani sum SHA-256.
- Decyzja: klient HTTP i podkomenda `pastiche download <nazwa>` powstaną
  w etapie 5, razem z pierwszymi modelami wymagającymi pobrania (SD 1.5,
  IP-Adapter z Hugging Face) i z ekranem akceptacji licencji OpenRAIL-M.
  Na Windowsie WinHTTP (bez nowej zależności), na Linuxie libcurl.

## D20. stable-diffusion.cpp + Vulkan pod MinGW: sonda potwierdzona (2026-09-13)

Pierwszy krok etapu 5 miał odpowiedzieć, czy D2 (dyfuzja na Vulkanie) w ogóle
się utrzyma, bo D2 nie ma wariantu zapasowego. Odpowiedź: **tak, buduje się
i liczy poprawnie**.

Zweryfikowane:

- Doinstalowane z MSYS2: `mingw-w64-x86_64-shaderc` 2026.3,
  `mingw-w64-x86_64-glslang` 16.3.0, `mingw-w64-x86_64-spirv-tools`
  oraz `mingw-w64-x86_64-spirv-headers`. Ten ostatni nie był w pierwotnym
  planie, a bez niego `ggml-vulkan` nie przechodzi configure
  (`Could not find SPIRV-HeadersConfig.cmake`). Vulkan headers i loader
  (1.4.335) były już obecne z czasów D2.
- Wersje sondy: sd.cpp `master-859-7f410a3`, ggml `e20c3a14`, GCC 15.2,
  CMake 4.2.1, Ninja. Klon w `third_party/sdcpp-probe/` (katalog ignorowany).
- Build statyczny `-DSD_VULKAN=ON -DSD_WEBP=OFF -DSD_WEBM=OFF`: exit 0, zero
  błędów, 9 ostrzeżeń, wszystkie nieszkodliwe (redefinicja `NOMINMAX`,
  `std::wstring_convert` deprecated, dwa fałszywie dodatnie
  `-Wstringop-overflow` w stb_image i `stl_uninitialized.h`).
- Build wariantu shared (`SD_BUILD_SHARED_LIBS=ON`, `GGML_NATIVE=OFF`,
  bez examples): też exit 0, `libstable-diffusion.dll` 106,8 MB.
- Runtime: `sd-cli.exe --list-devices` wykrywa `Vulkan0` = Intel UHD 630,
  `Vulkan1` = Quadro T2000, `CPU` = i9-9980HK.
- Poprawność liczenia na GPU: `test-backend-ops` z ggml uruchomiony na
  `Vulkan1` daje **16617/16617 tests passed, 0 FAIL** (3872 przypadki
  zgłoszone jako "not supported" to typy nieobsługiwane przez ten backend,
  m.in. część kwantyzacji i `LIGHTNING_INDEXER`; test je pomija, to nie są
  porażki). To jest dla dyfuzji odpowiednik tego, czym `--selftest` jest dla
  ścieżki ORT: realne porównanie GPU z referencją CPU op po opie.

Ustalenia o środowisku, które wyszły przy okazji:

- Klucza `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` na tym laptopie **nie ma**.
  To nie znaczy braku Vulkana: oba sterowniki rejestrują ICD nowszą metodą,
  przez `VulkanDriverName` w kluczu adaptera PnP
  (`...\Control\Class\{4d36e968-...}\0000` i `\0001`). Loader je znajduje.
  Nie diagnozować Vulkana po starym kluczu rejestru.
- Zależności DLL `sd-cli.exe`: tylko runtime MinGW (`libgcc_s_seh-1`,
  `libstdc++-6`, `libwinpthread-1`, `libgomp-1`) plus `vulkan-1.dll`, która
  jest częścią Windows. **Żadnego redistributable do dołożenia do paczki** -
  inaczej niż przy ONNX Runtime. `libgomp-1.dll` jest nowa względem etapów
  2-4, ale `package.sh` liczy zależności przez `ldd`, więc dołoży ją sama.
- ggml raportuje dla Quadro T2000 `matrix cores: none`. **To poprawny odczyt
  sprzętu, nie usterka.** Rozstrzygnięte sondą `third_party/vkprobe/`:
  sterownik NVIDIA 580.92 wystawia dla tej karty 228 rozszerzeń i nie ma
  wśród nich ani `VK_KHR_cooperative_matrix`, ani `VK_NV_cooperative_matrix`.
  Powód jest sprzętowy: Quadro T2000 Mobile to TU117GLM (PCI 10DE:1FB8),
  a TU117 - jak TU116 w GTX 1660 i jak GTX 1650 - jest wariantem Turinga
  **bez rdzeni tensor i RT**. NVIDIA zastąpiła w nim rdzenie tensor 128
  dedykowanymi jednostkami FP16 na SM, dającymi FP16 z podwójną
  przepustowością względem FP32. Nie ma więc czego wystawiać przez coopmat.
  Wykluczone po drodze: ggml nie blokuje NVIDII
  (`ggml_vk_khr_cooperative_matrix_support` ma dla niej `default: return
  true`; czarne listy dotyczą tylko starszych Intelów i AMD poza RDNA3),
  a glslc wspiera `GL_KHR_cooperative_matrix`, więc build nie jest okrojony.
- Wniosek dla wydajności: **szacunek "70-90 % wydajności CUDA" z D2 stoi.**
  Na TU117 CUDA też nie ma dostępu do rdzeni tensor, więc brak coopmat nie
  poszerza przepaści między Vulkanem a CUDA - obie ścieżki liczą na zwykłych
  jednostkach, z przyspieszeniem FP16 2x, które ggml wykrywa (`fp16: 1`).
  Na RTX 3060 (GA106, rdzenie tensor obecne) spodziewamy się
  `matrix cores: KHR_coopmat` i realnego przyspieszenia matmul - do
  potwierdzenia, gdy karta będzie pod ręką.

Rozstrzygnięcia dla reszty etapu 5:

1. **sd.cpp jako `stable-diffusion.dll` ładowana w locie, pakiet opcjonalny.**
   Shadery SPIR-V są wkompilowane w binarkę: `sd-cli.exe` ma 103 MB po strip,
   `libstable-diffusion.dll` 106,8 MB. Statyczne linkowanie do obu exe dałoby
   ~200 MB w ZIP-ie, więc odpada. Wybrany wariant to ten sam mechanizm co przy
   ONNX Runtime (D16): `LoadLibraryEx`, kolejność szukania
   `PASTICHE_SD_DIR`, katalog exe, ścieżka `third_party` z czasu kompilacji,
   `PATH`. Bazowe wydanie zostaje małe, a DLL jest osobnym pakietem obok wag
   SD 1.5, bo i tak nikt nie użyje dyfuzji bez pobrania 2 GB modelu. Gdy DLL
   nie ma, algorytm dyfuzyjny nie pojawia się w `--list-algos`, a próba użycia
   daje czytelny błąd - tak jak dziś Johnson bez ORT.
2. **Wybór urządzenia jawny.** Domyślnie pierwsze urządzenie to iGPU Intela,
   nie Quadro. Potrzebny wybór dyskretnej karty, odpowiednik filtra
   `HighPerformance` z DirectML, plus parametr do nadpisania. Nazwy urządzeń
   bierzemy z tego samego API, co `sd-cli --list-devices`.
3. **GGML_NATIVE=OFF w wydaniu.** Domyślnie ggml kompiluje CPU backend
   z `-march=native`, co dałoby paczkę działającą tylko na maszynach
   podobnych do budującej.
4. **FetchContent z przypiętym commitem**, zgodnie z D10. sd.cpp ma własne
   submoduły (ggml, libwebp, libwebm, frontend servera), więc trzeba wyłączyć
   examples, servera i duplikat libwebp - swoje libwebp mamy z MSYS2 (D6).
   (Zmienione w D21 na build poza drzewem - patrz niżej.)

## D21. sd.cpp budowana poza drzewem, nie przez FetchContent (2026-09-13)

Punkt 4 z D20 mówił "FetchContent z przypiętym commitem", zgodnie z listą
zależności w D10. Przy wpinaniu okazało się to złym wyborem i zostaje
zmienione: `stable-diffusion.cpp` jest budowana **poza drzewem projektu**
skryptem `tools/build_sdcpp.sh`, który zostawia gotowce w
`third_party/stable-diffusion/{include,bin}`. Główny `CMakeLists.txt` widzi
z tego wyłącznie nagłówek `stable-diffusion.h`; DLL nigdy nie jest linkowana,
tylko ładowana w locie (D20 pkt 1).

Powody:

- **Kolizja libwebp.** sd.cpp wciąga libwebp i libwebm jako własne submoduły.
  Wstawienie tego przez `add_subdirectory` wprowadziłoby do naszego drzewa
  drugie libwebp obok tego z MSYS2, którego używamy od D6. Trzymanie sd.cpp
  poza drzewem znosi problem w całości, zamiast go obchodzić flagami.
- **Czas builda.** Kompilacja shaderów Vulkana to ok. 370 celów i kilka minut.
  `cmake --build build` ma zostać szybkie; DLL zmienia się raz na kilka
  miesięcy, przy podbiciu wersji sd.cpp.
- **Spójność z ORT.** Po decyzji z D20 pkt 1 sd.cpp jest strukturalnie tym
  samym co ONNX Runtime: obcą biblioteką ładowaną w locie, z nagłówkami
  w `third_party/` i binariami spoza repozytorium (D15, D16). Niech więc
  będzie obsługiwana tak samo, zamiast trzecim mechanizmem.
- Przypięcie wersji nie ucieka: commit sd.cpp jest zapisany w skrypcie,
  w jednym miejscu, tak jak tagi ImGui i nfd są w `CMakeLists.txt`.

Gdy nagłówka nie ma, configure nie pada - wypisuje, że dyfuzja jest wyłączona
i jak ją włączyć, a algorytm dyfuzyjny nie trafia do builda. To ta sama
uprzejmość, co przy braku libjxl (D6).

Wnioski z lektury `include/stable-diffusion.h`, które upraszczają resztę etapu:

- `sd_list_devices()` zwraca `nazwa<TAB>opis` po jednej linii na urządzenie -
  dokładnie to, czego potrzebuje wybór dyskretnego GPU z D20 pkt 2. Nazwy
  (`vulkan0`, `vulkan1`, `cpu`) idą wprost do pola `backend`
  w `sd_ctx_params_t`.
- `sd_cancel_generation()` istnieje, więc anulowanie dyfuzji **nie potrzebuje
  wątku-strażnika** z D18. Tamten obchodził brak możliwości przerwania
  pojedynczego `Run()` w ORT; sd.cpp przerywa się sam między krokami.
- `sd_set_preview_callback(cb, mode, interval, ...)` daje podgląd pośredni
  z D9 gotowy, bez dłubania w pętli samplera.
- `sd_ctx_params_t.max_vram` przyjmuje budżet w GiB per urządzenie. To nie
  zastępuje preflightu z D5 - nadal liczymy estymację i odmawiamy przed
  załadowaniem modelu - ale jest drugą linią obrony.

## D22. Downloader modeli: katalog, sumy z API, wznawianie (2026-09-13)

D19 odłożył downloader, bo `models.json` nie miałby prawdziwych URL-i ani sum
SHA-256. Oba problemy są rozwiązane, podkomenda `pastiche download` działa.

**Sumy kontrolne bez pobierania czegokolwiek.** Hugging Face przechowuje duże
pliki w Git LFS, a `lfs.oid` obiektu LFS **jest** sumą SHA-256 zawartości.
Da się je wyciągnąć jednym zapytaniem:

    https://huggingface.co/api/models/<repo>/tree/main?recursive=1

Cały katalog został wypełniony tą drogą, bez ściągania 6,4 GB tylko po to, żeby
policzyć hashe. Potwierdzone empirycznie: pobrany `ip-adapter_sd15.safetensors`
ma dokładnie sumę, którą podało API. To także znaczy, że przy podbiciu wersji
modelu nie trzeba niczego liczyć lokalnie.

**Zakres modeli (D7):** `sd15` (v1-5-pruned-emaonly.safetensors, 4,0 GiB,
OpenRAIL-M, wymaga akceptacji), `ip-adapter-sd15` (42,6 MiB, Apache 2.0),
`clip-vision` (enkoder obrazu ViT-H/14, 2,4 GiB, Apache 2.0). Razem 6,4 GiB,
nic z tego nie wchodzi do wydania.

**Realizacja:**

- Własne SHA-256 (`src/core/sha256.*`) zamiast CNG na Windows i OpenSSL gdzie
  indziej - 150 linii kontra dwie zależności platformowe. Testy na wektorach
  z FIPS 180-4, łącznie z milionem znaków `a` i granicami dopełnienia.
- Własny parser JSON tylko do odczytu (`src/core/json.*`). Zapis JSON-a
  projekt miał od dawna (sidecar, params), brakowało wyłącznie czytania.
  Komunikaty błędów podają wiersz i kolumnę, bo ręcznie edytowany katalog
  dostaje przecinek na końcu listy.
- HTTP: WinHTTP na Windows (część systemu, nic do dołożenia do paczki),
  libcurl na Linuxie przez `find_package(CURL)`; bez niej build przechodzi,
  a pobieranie mówi, że jest niedostępne - zgodnie z "teoretycznym" Linuxem
  z D10.
- Transfer idzie do `<plik>.part` i jest przemianowywany dopiero po sukcesie,
  więc przerwane pobranie nigdy nie wygląda jak skończone. Wznawianie przez
  nagłówek `Range`; gdy serwer go zignoruje i odpowie 200 zamiast 206,
  plik częściowy jest odrzucany, zamiast skleić dwie połówki w śmieć.
  Sprawdzone podłożeniem 20 MiB jako `.part`: doklejone, suma się zgadza.
- Walidacja katalogu przy wczytaniu, nie w trakcie pobierania: URL musi być
  https, suma 64 znakami hex, rozmiar niezerowy, a ścieżka docelowa nie może
  wyjść poza katalog modeli (`..` odrzucane - inaczej wpis w katalogu byłby
  sposobem na nadpisanie dowolnego pliku).
- Licencja nieprzemisywna: streszczenie zawijane do 74 kolumn, link do pełnego
  tekstu, zastrzeżenie że to streszczenie, i pytanie z domyślną odpowiedzią
  NIE. `--yes` dla instalacji skryptowej.

## D23. Precyzja wag: domyślnie f16, nie q8_0. I dlaczego selftest z D12 nie zadziała dla dyfuzji (2026-09-13)

D7 zakładał "na 4 GB: SD 1.5 w Q8". Pomiar to podważa - **na T2000 lepszy jest
f16** - a przy okazji wyszła rzecz poważniejsza, dotycząca sposobu testowania.

### Pomiar

Ten sam prompt, ziarno 42, 512 px, 8 kroków, `sd-cli` z sondy.

| wariant | wagi | czas |
|---|---|---|
| Vulkan f16 | 2111 MB | **24,9 s** |
| Vulkan q8_0 | ok. 1300 MB | 29,6 s |
| CPU f32 | 2784 MB RAM | 176 s |
| CPU f16 | 2111 MB RAM | 172 s |
| CPU q8_0 | 1796 MB RAM | 166 s |

**f16 jest na tej karcie szybszy od q8_0 mimo większego zużycia pamięci.**
Spójne z D20: TU117 nie ma rdzeni tensor, ale ma dedykowane jednostki FP16
o podwójnej przepustowości względem FP32. q8_0 musi dodatkowo dekwantyzować
przed każdym mnożeniem i to zjada zysk z mniejszego transferu.

Odległość od pełnej precyzji (procesor, ten sam backend, więc izolowany wpływ
samej precyzji), miarą z `tools/compare_images.py`:

| porównanie | średnia różnica | PSNR |
|---|---|---|
| CPU f16 vs CPU f32 | 0,46/255 | 43,3 dB |
| CPU q8_0 vs CPU f32 | 6,72/255 | 25,9 dB |

f16 trzyma się referencji o rząd wielkości bliżej. Zgodne z matematyką z D22:
w q8_0 błąd jest odniesiony do maksimum 32-elementowego bloku, więc mała waga
obok dużej cierpi nieproporcjonalnie; w f16 każda liczba ma własną precyzję
rzędu 0,05 % swojej wartości, bez efektu sąsiedztwa.

### Wniosek, który wywraca plan testów z D12

**Te liczby NIE są miarą jakości.** Dowód jest w samych pomiarach:

| porównanie | średnia różnica | PSNR |
|---|---|---|
| Vulkan f16 vs CPU f16 | 9,23/255 | 23,8 dB |

To jest **ta sama precyzja na dwóch backendach** i różnica wychodzi *większa*
niż między f32 a q8_0. Oglądnięcie obrazów potwierdza: wszystkie trzy są
równie dobre, ten sam samochód, ta sama kompozycja, różnią się kształtem gór
w tle i detalami. Żaden nie jest gorszy - są to różne, równie poprawne próbki.

Powód jest wpisany w metodę: sampling dyfuzyjny jest iteracyjny i chaotyczny,
więc różnica daleko poniżej precyzji wag wystarczy, żeby po kilku krokach
tor pobiegł do innego obrazu.

Stąd twarda konsekwencja dla etapu 5:

- **`--selftest` dla dyfuzji nie może porównywać pikseli z referencją CPU
  z tolerancją, tak jak robi to dla Johnsona i AdaIN** (D12: "suma kontrolna
  vs referencja CPU EP z tolerancją", dziś mean diff 0.00 przy progu 2.0).
  Dla dyfuzji taki test failowałby zawsze, a jego zaostrzanie byłoby gonieniem
  wiatru.
- Zamiast tego: rozmiar, brak NaN, obraz nie jest czarny ani jednolity,
  wartości w zakresie, i - to ma sens - że wynik *nie jest* identyczny
  z wejściem, bo to wykryłoby pipeline, który tylko przepisuje treść.
- Porównanie z referencją zostaje jako narzędzie diagnostyczne
  (`tools/compare_images.py`), nie jako test przechodzi/nie przechodzi.

### Decyzja

- **Domyślna precyzja dyfuzji: f16.** Szybsza na T2000, bliższa referencji,
  mieści się przy 512 px.
- **q8_0 jako zejście awaryjne, nie domyślne.** 2111 MB wag przy realnie
  ok. 3300 MB wolnych zostawia mało miejsca: przy obu wariantach dekodowanie
  VAE nie zmieściło się i silnik sam przeszedł na kafelkowanie przestrzenne.
  Powyżej 512 px f16 przestanie się mieścić i wtedy q8_0 wraca.
- Precyzja idzie jako parametr algorytmu (ParamSpec, D4), a preflight z D5
  liczy estymację osobno dla każdego wariantu i przy odmowie sugeruje zejście
  na q8_0 zanim zaproponuje zmniejszenie rozdzielczości.
- Zastrzeżenie: to jeden prompt i jedno ziarno przy 8 krokach. Ranking
  szybkości i odległości od referencji jest wyraźny i zgodny z teorią, ale
  gdyby kiedyś zależało od tego coś kosztownego, trzeba powtórzyć na serii.
