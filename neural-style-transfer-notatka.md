# Neural Style Transfer (NST) - notatka

Data notatki: 2026-09-12

## Kiedy powstał

Neural Style Transfer wprowadzili Leon Gatys, Alexander Ecker i Matthias Bethge
w pracy "A Neural Algorithm of Artistic Style". Preprint trafił na arXiv
26 sierpnia 2015 (arXiv:1508.06576), pełna wersja ukazała się na CVPR 2016.

Idea: wziąć wytrenowaną sieć VGG (VGG-16 / VGG-19, ImageNet), opisać "treść"
obrazu aktywacjami w głębokich warstwach, a "styl" macierzami Grama korelacji
cech w kilku warstwach, i optymalizować piksele obrazu wyjściowego tak, żeby
pasował do obu (suma ważona content loss + style loss).

## Warianty i złożoność obliczeniowa

Pierwotna metoda była wolna, późniejsze warianty przyspieszyły ją o rzędy
wielkości.

### 1. Gatys 2015 - metoda optymalizacyjna

- Każdy obraz to osobna optymalizacja gradientowa (L-BFGS lub Adam),
  typowo 500-1000 iteracji przez VGG-19.
- Zaleta: dowolny styl bez trenowania, wysoka jakość.
- Wada: sekundy do minut na obraz, nawet na dobrym GPU.

### 2. Johnson 2016 "Perceptual Losses" / Ulyanov 2016 "Texture Networks" - feed-forward, jeden styl

- Trenuje się małą sieć generującą (encoder-decoder z blokami residualnymi),
  która robi transfer w jednym przebiegu. Funkcja straty ta sama co u Gatysa
  (liczona przez zamrożony VGG).
- Ok. 1000x szybciej od Gatysa w inferencji.
- Jedna sieć obsługuje jeden styl. Trening nowego stylu trwa godziny
  (MS COCO ~80k obrazów, 2 epoki).
- Ulyanov dodał Instance Normalization ("Instance Normalization: The Missing
  Ingredient for Fast Stylization", 2016), co mocno poprawiło jakość.

### 3. Huang, Belongie 2017 "AdaIN" - feed-forward, dowolny styl

- Jedna sieć dla dowolnego stylu w czasie rzeczywistym (ICCV 2017).
- Adaptive Instance Normalization: dopasowuje średnią i wariancję cech treści
  do cech stylu, potem dekoder odtwarza obraz.
- Autorzy podają ok. 720x przyspieszenia względem Gatysa.
- Pozwala na interpolację stylów, kontrolę content/style trade-off,
  kontrolę kolorów i przestrzenną.

### 4. 2022-2025 - metody dyfuzyjne

- StyleID (CVPR 2024, training-free), InstantStyle (2024), InstantStyle-Plus,
  StyleSSP (CVPR 2025), DiffuseST i inne. Bazują na Stable Diffusion / SDXL.
- Pozwalają na "deformowalny" transfer stylu (zmiana kształtów, nie tylko
  tekstury), czego klasyczny NST nie potrafił.
- Koszt obliczeniowy znacznie wyższy niż AdaIN (dziesiątki kroków dyfuzji
  dużym modelem).

### Orientacyjne czasy (z dokumentacji oryginalnych repozytoriów)

| Metoda                                          | Sprzęt             | Czas         |
|-------------------------------------------------|--------------------|--------------|
| Gatys, 512 px, 500 iteracji, L-BFGS (nn)        | Titan X (Maxwell)  | 62 s         |
| Gatys, 512 px, 500 iteracji, cuDNN + Adam       | Titan X (Maxwell)  | 44 s         |
| Gatys, 512 px, 500 iteracji, cuDNN + Adam       | Titan X (Pascal)   | 22 s         |
| Johnson fast-neural-style, 1200x630             | Titan X (Pascal)   | 50 ms        |
| Johnson, webcam 640x480                         | Titan X (Pascal)   | 4 modele realtime |
| AdaIN, 512x512                                  | Titan X (Pascal)   | ok. 15 FPS   |
| Optymalizacyjny vs. feed-forward, 512 px (2022) | RTX 2080 Ti        | 38 s vs 4.5 s |

## Wymagania GPU

Klasyczny NST jest skromny w porównaniu z dzisiejszymi modelami generatywnymi.

- Gatys, 512 px: ok. 3.5 GB VRAM domyślnie (backend nn + L-BFGS),
  ok. 1 GB z cuDNN + Adam. Wystarczy karta z 4 GB. Na współczesnej karcie
  klasy RTX 3060 / 4060 spodziewać się kilku do kilkunastu sekund na obraz.
- Wyższa rozdzielczość: pamięć rośnie ok. kwadratowo z rozmiarem obrazu.
  Jedno źródło podaje 7.2 GB dla 1024x1024 i 38.6 GB dla 3500x3500.
  Obejście: -image_size 256 lub kafelkowanie.
- Feed-forward (Johnson, AdaIN): inferencja mieści się w 2-4 GB, działa
  w czasie rzeczywistym na kartach klasy średniej, a nawet na urządzeniach
  mobilnych (po konwersji do CoreML / TFLite / ONNX).
- Trening własnego stylu (Johnson / AdaIN): kilka godzin na karcie
  z co najmniej 8-12 GB (autorzy testowali na Titan X 12 GB).
- CPU: działa, ale 10-100x wolniej; dla Gatysa to minuty na obraz.
  Feed-forward na CPU jest używalny (sekundy na obraz w 512 px).
- Metody dyfuzyjne (SDXL): typowo 12-24 GB VRAM dla wygodnej pracy.

## Licencje kodu

Same algorytmy są opisane w publicznych pracach naukowych i nie są
opatentowane w sposób blokujący użycie. Kod referencyjny ma różne licencje:

| Repozytorium                        | Metoda   | Framework | Licencja |
|-------------------------------------|----------|-----------|----------|
| jcjohnson/neural-style              | Gatys    | Torch/Lua | MIT |
| jcjohnson/fast-neural-style         | Johnson  | Torch/Lua | "Free for personal or research use; for commercial use please contact me" (NIE open source) |
| DmitryUlyanov/texture_nets          | Ulyanov  | Torch/Lua | Apache 2.0 |
| xunhuang1995/AdaIN-style            | AdaIN    | Torch/Lua | MIT |
| naoto0804/pytorch-AdaIN (nieoficj.) | AdaIN    | PyTorch   | MIT (sprawdzić LICENSE) |
| PyTorch tutorial "Neural Transfer"  | Gatys    | PyTorch   | BSD-3 |
| pytorch/examples fast_neural_style  | Johnson  | PyTorch   | BSD-3 |
| jiwoogit/StyleID                    | dyfuzja  | PyTorch   | sprawdzić LICENSE |

Zastrzeżenie: większość implementacji korzysta z wag VGG-16/VGG-19
wytrenowanych na ImageNet. Same wagi VGG (Oxford) są na CC BY 4.0, ale zbiór
ImageNet ma ograniczenia do celów niekomercyjnych, co czasem bywa podnoszone
przy produktach komercyjnych. Dla bezpieczeństwa licencyjnego można użyć
implementacji PyTorch (BSD) + wag torchvision.

## Porównanie wizualne: ile tracimy bez Gatysa

Materiały (figury wyrenderowane z prac) leżą w `research/cmp_*.png`.

Ustalenia z literatury:

- Jing et al. (TVCG 2020), Tab. 3: Gatys "good and usually regarded as a gold
  standard"; wyniki Johnsona i Ulyanova "close to [Gatys]"; metody dowolnego
  stylu (ASPM: AdaIN, WCT, Chen&Schmidt) "less impressive than other types",
  AdaIN "generally not effective at producing complex style patterns",
  WCT "not good at producing sharp details and fine strokes".
- Johnson 2016, Fig. 5: strata jednego przebiegu sieci odpowiada 50-100
  iteracjom L-BFGS Gatysa (na 500). Fig. 6: wyniki "qualitatively similar".
- AdaIN 2017, Fig. 3-4: strata stylu AdaIN i Ulyanova podobna do Gatysa po
  50-100 iteracjach; autorzy przyznają, że dla niektórych par są "slightly
  behind" Ulyanova i Gatysa.
- NNST 2022, Fig. 3-4: wszystkie metody neuronowe "fail to entirely capture
  many styles' long range correlation of textural features and high frequency
  details". Gatys w tym zestawieniu wypada gorzej niż STROTSS i NNST-Opt.
- StyleID (CVPR 2024), Fig. 6: metody dyfuzyjne lepiej zachowują strukturę
  treści i przenoszą styl niż AdaIN / AesPA-Net / StyTr2; czas 12.4 s na parę
  na Titan RTX (SD 1.x, DDIM inversion 8.2 s + sampling 4.2 s).

Czasy z Jing et al., Tab. 2 (Quadro M6000, 512x512): Gatys 51.19 s,
Johnson 0.045 s, Ulyanov 0.047 s, AdaIN 0.095 s (0.137 s z kodowaniem stylu),
WCT 1.139 s. Dla 1024x1024: Gatys 200.3 s, Johnson 0.166 s, AdaIN 0.382 s.

Wniosek roboczy: Johnson/Ulyanov (jeden styl na model) są wizualnie prawie
nieodróżnialne od Gatysa. AdaIN jest zauważalnie słabszy przy złożonych
wzorach stylu i drobnych pociągnięciach. Dyfuzja (StyleID, InstantStyle)
przebija klasyczne metody jakościowo, ale kosztem czasu i VRAM. Gatysa można
odłożyć; jeżeli wróci, to raczej w wersji NNST-Opt lub STROTSS niż oryginalnej.

## Źródła

- Gatys et al., arXiv 1508.06576: https://arxiv.org/abs/1508.06576
- jcjohnson/neural-style: https://github.com/jcjohnson/neural-style
- jcjohnson/fast-neural-style: https://github.com/jcjohnson/fast-neural-style
- Johnson et al., Perceptual Losses, ECCV 2016: https://link.springer.com/chapter/10.1007/978-3-319-46475-6_43
- Ulyanov et al., Texture Networks, ICML 2016: https://proceedings.mlr.press/v48/ulyanov16.html
- DmitryUlyanov/texture_nets: https://github.com/DmitryUlyanov/texture_nets
- Huang, Belongie, AdaIN, ICCV 2017: https://openaccess.thecvf.com/content_ICCV_2017/papers/Huang_Arbitrary_Style_Transfer_ICCV_2017_paper.pdf
- xunhuang1995/AdaIN-style: https://github.com/xunhuang1995/AdaIN-style
- naoto0804/pytorch-AdaIN: https://github.com/naoto0804/pytorch-AdaIN
- Neural Neighbor Style Transfer (czasy RTX 2080 Ti): https://arxiv.org/pdf/2203.13215
- VRAM dla dużych rozdzielczości: https://www.ncbi.nlm.nih.gov/pmc/articles/PMC8400862/
- jiwoogit/StyleID, CVPR 2024: https://github.com/jiwoogit/StyleID
- InstantStyle, arXiv 2404.02733: https://arxiv.org/pdf/2404.02733
- Style Transfer: A Decade Survey (2025): https://arxiv.org/html/2506.19278v1
- Awesome Style Transfer with Diffusion Models: https://github.com/Westlake-AGI-Lab/Awesome-Style-Transfer-with-Diffusion-Models
