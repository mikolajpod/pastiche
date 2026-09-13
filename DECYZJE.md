# ML-Styler - dziennik decyzji projektowych

Data rozpoczęcia: 2026-09-12

## Ustalenia o środowisku

- Laptop: Quadro T2000 4 GiB (Turing, compute 7.5, tensor cores, FP16),
  driver 580.92, i9-9980HK, 64 GB RAM. Docelowo też RTX 3060 12 GiB.
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
