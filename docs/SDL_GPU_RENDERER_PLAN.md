# Renderizador nativo sobre a API de GPU do SDL3 (`sdl_gpu`) — plano

Branch do framework: `feat/sdl-gpu-renderer` (worktree `K:\src\psxrecomp-gpu`, a partir do
`f3480ca7`, o framework do build-dev em 2026-10-31). Jogo para compilar e testar: worktree
`K:\src\bof4-gpu` (branch `feat/sdl-gpu-renderer`, a partir do `d379e1a`; `psxrecomp` é junção
para `K:\src\psxrecomp-gpu`; `generated/` copiado do main), diretório de build `build-gpu`.

Objetivo: um renderizador nativo (Vulkan, Direct3D 12 e Metal pelo `SDL_GPU` do SDL 3.4) que
iguale o OpenGL atual em tudo, inclusive todos os efeitos do BoF4, no menor tempo possível. O
OpenGL continua o padrão e a referência pixel a pixel até o novo igualar.

## 1. A decisão de arquitetura: o MESMO código do renderizador, sobre uma camada GL→SDL_GPU

O renderizador OpenGL não é só "chamadas de GL": são ~16,9 mil linhas (`gpu_gl_renderer.c`) +
~5,4 mil nos `.inc` com toda a lógica do PS1 (VRAM autoritativa na GPU, bit de máscara no
stencil, espelho R16UI, lotes HLE/LLE, coerência CPU↔GPU) e de TODOS os efeitos (marcas 3D,
captura da cena para as sombras, sprites, luzes do fogo, cadeias `.pfx`, 16:9, geração de
quadros, HDR...). As chamadas de GL são ~2.500 pontos, mas só **~75 funções distintas** do GL
3.3 core (GLSL `#version 330` em todos os shaders). Medido:

- funções modernas (ponteiros `p_gl*`): BindFramebuffer 250, GetUniformLocation 235, Uniform*
  ~320, UseProgram 100, ActiveTexture 95, BindVertexArray 76, BindBuffer 43, BlitFramebuffer 39,
  BufferData 25, BlendEquationSeparate 21, BlendFuncSeparate 13, queries de tempo ~60, fences 16,
  MapBuffer 4, FramebufferTexture2D 19, Renderbuffer* ~30...
- GL 1.1: Disable 137, BindTexture 129, TexParameteri 96, GetIntegerv 70, Viewport 65,
  DrawArrays 64, Enable 59, PixelStorei 50, DeleteTextures 45, Clear 37, TexImage2D 32,
  ColorMask 32, ReadPixels 28, Scissor 19, IsEnabled 19, StencilMask/Func/Op ~38, DepthMask/Func
  ~26, TexSubImage2D 11, CopyTexSubImage2D 6, LineWidth 4, GetTexImage 2.

Portar à mão cada um desses pontos para o modelo do SDL_GPU (passes de render, pipelines fechados,
cópias fora dos passes) seria reescrever o renderizador inteiro e manter DUAS implementações de
cada efeito para sempre. O caminho mais rápido até a paridade completa é outro:

**Uma camada própria, `sgl`, que implementa esse subconjunto do GL 3.3 core sobre o SDL_GPU, e
o renderizador atual chamando o GL por uma tabela de ponteiros.** Com `[video] renderer =
"opengl"` a tabela aponta para o driver OpenGL (comportamento idêntico ao de hoje: só uma
indireção); com `renderer = "sdl_gpu"` ela aponta para a `sgl`. Um só código de renderizador,
uma só fonte de shaders, todo efeito novo vale nos dois.

Por que isso não é "OpenGL emulado lento":

- A `sgl` **grava** as chamadas numa lista própria (barata: cópias de memória) e só **codifica**
  no SDL_GPU em lote, quando precisa (troca de tela, leitura da GPU, `glFinish`): todos os
  vértices do trecho sobem numa única passada de cópia, os desenhos seguidos no mesmo alvo viram
  um único passe de render, os pipelines saem de um cache por estado. É o modelo que o backend
  Vulkan antigo teve de adotar para não morrer de "submit churn" (`docs/ENHANCEMENTS.md` R2:
  milhares de submits por quadro → ≤6).
- No turbo a thread de render passa ~97% do tempo dentro do driver OpenGL. Com a `sgl` o custo
  vira: a mesma lógica do renderizador + gravação + codificação Vulkan (sem a validação e as
  sincronizações implícitas do driver GL). A etapa (g) mede e otimiza isso.
- A geração de quadros dos drivers (NVIDIA Smooth Motion, lsfg-vk) e a portabilidade
  (Linux/Deck/Mac) vêm do swapchain Vulkan/D3D12/Metal, que é o que a `sgl` usa.

O backend Vulkan experimental (`gpu_vk_renderer.c`, ~3,6 mil linhas, sem nenhum efeito) não é a
base: copiá-lo exigiria reescrever todos os efeitos de novo. Aproveita-se dele as lições
(uploads em lote, sub-alocação de vértices, semitransparência em duas passadas, barreira do
stencil) e o `tools/embed_spirv.py`. Ele continua no código, escondido, sem mudanças.

### 1.1 Peças novas no framework

| Arquivo | O que faz |
|---|---|
| `runtime/src/gl_api.h` | tabela de ponteiros de TODAS as funções de GL que o renderizador usa (as do GL 1.1 também) e das `SDL_GL_*` de contexto/janela; macros `glX` → `p_glX` incluídas só pelo renderizador e pelos `.inc` |
| `runtime/src/gl_api_native.c` | preenche a tabela com o OpenGL de verdade (`SDL_GL_GetProcAddress` + os símbolos do opengl32) |
| `runtime/src/sgl/sgl.h` | a API da camada (mesmas assinaturas do GL) e `sgl_caps` |
| `runtime/src/sgl/sgl_state.c` | objetos (texturas, renderbuffers, framebuffers, programas, buffers, VAOs, queries, fences) e o estado do GL 3.3 que o renderizador usa |
| `runtime/src/sgl/sgl_record.c` | gravação e codificação: passes de render, passadas de cópia, cache de pipelines e samplers, uniformes por desenho, arena de vértices, leituras síncronas e assíncronas |
| `runtime/src/sgl/sgl_shader.cpp` | GLSL 330 → SPIR-V (glslang, em tempo de execução, com cache em disco), reflexão dos uniformes; SPIR-V → HLSL (D3D12) / MSL (Metal) pelo SPIRV-Cross |
| `runtime/src/sgl/sgl_window.c` | dispositivo, janela reivindicada, swapchain (VSync/HDR), "framebuffer padrão" emulado, troca de tela |
| `gpu_gl_renderer.h` | `gl_renderer_set_impl(0/1)`, `gl_renderer_impl()`, `gl_renderer_impl_name()`, `gl_renderer_impl_window_flags()`, `gl_renderer_sgl_json()`: o backend do emulador continua `GR_BACKEND_OPENGL` (é o mesmo código), só a implementação do GL muda |
| `recompiler/src/config_loader.cpp`, `main.cpp`, settings | `[video] renderer = "sdl_gpu"`, `[video] sdl_gpu_driver = "auto" \| "vulkan" \| "direct3d12" \| "metal"`, CLI `--renderer sdl_gpu` |

Nada muda para quem usa OpenGL: mesma tela, mesmas medidas (a etapa (a) confere).

### 1.2 Como cada coisa do GL vira SDL_GPU (a parte difícil)

| GL | Na `sgl` |
|---|---|
| framebuffer 0 (janela) | textura própria do tamanho da janela (RGBA8; RGBA16F com HDR); a troca de tela copia para o swapchain **invertendo Y** (no GL a linha 0 da memória é a de baixo da tela; no Vulkan, a de cima) |
| orientação | mantida a do GL em memória: NDC → linhas de memória é igual nos dois, `gl_FragCoord`, viewport, scissor, `glReadPixels`, `TexSubImage` usam os mesmos números; só a apresentação inverte. Nenhum shader muda |
| profundidade NDC [-1,1] | o shader de vértice ganha, depois do `main` original, `gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5` (o `main` do shader vira outro nome por `#define`; não se edita shader) |
| uniforms soltos | glslang em modo "Vulkan relaxado" junta todos num bloco por estágio (vértice: set 1, fragmento: set 3, como o SDL_GPU exige); `glGetUniformLocation` = índice num catálogo por programa (pela reflexão); `glUniform*` grava numa cópia na CPU; o desenho empurra o bloco (`SDL_PushGPU*UniformData`) só quando mudou |
| `sampler2D` + `glUniform1i(unidade)` | ligações numeradas automaticamente (set 0/2); no desenho cada ligação recebe a textura e o sampler da unidade indicada pelo uniforme |
| `glTexParameteri` (filtro, wrap) | guardado na textura; sampler do SDL_GPU de um cache por combinação |
| estado de blend, color mask, depth, stencil (máscaras, ops, função), primitiva, formatos dos alvos, layout dos vértices | chave do pipeline (cache por hash); viewport, scissor, cor de blend e referência do stencil são dinâmicos |
| `glClear` com scissor | desenho de um retângulo que escreve cor/profundidade/stencil (o `LOAD_OP_CLEAR` do SDL_GPU limpa o alvo inteiro); `CLEAR` de verdade quando o scissor cobre tudo |
| `glBufferData` + `glDrawArrays` | os vértices vão para a arena do trecho; uma passada de cópia antes dos passes de render; o desenho usa o deslocamento na arena |
| `glTexSubImage2D` (upload de VRAM, staging) | buffer de transferência; passada de cópia NA ORDEM (fecha o passe de render se estiver aberto) |
| `glBlitFramebuffer` de cor | `SDL_BlitGPUTexture` (escala, filtro, inversão) ou cópia direta quando 1:1 |
| `glBlitFramebuffer` de profundidade (cadeias `.pfx`: escala e inversão) | desenho que lê a profundidade como textura e escreve `gl_FragDepth` |
| `glBlitFramebuffer` só de stencil (geração de quadros, 1:1) | cópia profundidade+stencil + desenho que devolve a profundidade original do destino |
| `glCopyTexSubImage2D` | cópia de textura (mesmo formato) ou blit |
| `glReadPixels` / `glGetTexImage` | codifica tudo até ali, baixa para buffer de transferência, espera a fence (igual ao GL, que também para) |
| PBO de leitura + `glFenceSync` + `glMapBuffer` | download assíncrono; o mapa espera a fence daquele download |
| `glLineWidth(S)` + `GL_LINES` | linhas largas viram dois triângulos na CPU (o SDL_GPU só tem linha de 1 px), deslocadas no eixo menor como no GL |
| `glFinish` / `glFlush` | submete e espera / submete |
| `glGetIntegerv`, `glIsEnabled`, `glGetError` | respondidos pelo estado guardado (`GL_NO_ERROR`) |
| limites (`GL_MAX_TEXTURE_SIZE`...) | 16384 (o SDL_GPU não informa; é o limite do D3D12); o renderizador já tem a janela de alta resolução para o 8K |
| contextos (render thread, `MakeCurrent`) | um "dono" da camada por vez; o swapchain é tocado por quem troca a tela (Windows/Linux); no macOS a troca vai para a thread da janela (risco R6) |

**O que o SDL_GPU não tem** e como fica (o renderizador lê `sgl_caps`; com OpenGL tudo segue
como hoje):

1. **Blend de duas fontes** (`GL_SRC1_ALPHA`): o lote HLE (`semi == 4`, opaco + semitransparente
   modos 0/1/3 num só desenho) e as bordas suaves dos filtros dependem dele. Sem ele: política
   de lotes nova `NODUAL` (escolhida em tempo de execução): cada peça semitransparente sai
   isolada nas duas passadas que já existem (STP=0 opaco, STP=1 com o blend do modo) — os
   mesmos pixels, mais desenhos. Bordas "Blend" dos filtros caem para "Clean" (como hoje onde o
   passe não usa duas fontes). Custo medido na etapa (g) (o dragão da ravina é o pior caso).
2. **Consultas de tempo da GPU** (`postfx_timing`, `frame_perf`, medidor F): o SDL_GPU não tem.
   Na etapa (g): tempo por fence (envio → sinal) por trecho, só com a ferramenta pedindo; até
   lá o TCP responde `"unavailable": "sdl_gpu"`.
3. **Linha larga**: expandida na CPU (acima).

## 2. Shaders: um conjunto só

- Os GLSL de hoje (os ~44 programas embutidos no renderizador e as `.pfx`/`.glsl` de
  `shaders/` do framework e do jogo, carregados e trocados ao vivo pelo TCP) continuam
  **exatamente** como estão. Não há segunda versão de shader.
- A `sgl` compila GLSL 330 → SPIR-V em tempo de execução com o **glslang** (Khronos; BSD-3/MIT/
  Apache-2.0) ligado ao executável, regras "Vulkan relaxado" (`setEnvInputVulkanRulesRelaxed`,
  `setGlobalUniformSet/Binding`, `setAutoMapBindings`, `setAutoMapLocations`): uniforms soltos
  viram um bloco, ligações e locations numeradas automaticamente. Cache em disco (hash da fonte
  + versão da camada → SPIR-V) em `shader_cache/` ao lado do exe: a 2ª execução não compila.
- Vulkan usa o SPIR-V direto. **D3D12**: SPIR-V → HLSL (SPIRV-Cross, Apache-2.0) → DXBC SM 5.1
  pelo `d3dcompiler_47.dll` que vem com o Windows (sem DXC, sem dependência extra).
  **Metal**: SPIR-V → MSL (SPIRV-Cross), que o SDL_GPU aceita como fonte.
- O `SDL_shadercross` foi considerado: para D3D12 ele depende do DXC (`dxcompiler.dll`, ~17 MB)
  e faz o mesmo SPIRV-Cross por baixo; o caminho acima evita o DXC.
- **Build sem rede em tempo de execução**: os fontes do glslang e do SPIRV-Cross ficam em
  `third_party/` (tarball com hash no `deps.manifest`, como o libchdr), compilados pelo CMake
  como bibliotecas estáticas (glslang sem SPIRV-Tools/otimizador: `ENABLE_OPT=OFF`). Opção
  `PSX_SDL_GPU` (padrão ON quando os fontes estão presentes); sem eles o backend não é
  compilado e `renderer = "sdl_gpu"` cai para OpenGL com aviso.
- **Pendente da usuária (perguntado em 2026-10-31):** baixar uma vez esses dois fontes. O
  toolchain não tem nenhum compilador GLSL (nem `glslc`: por isso o backend Vulkan antigo nunca
  foi compilado aqui). Sem eles a etapa (a) para no ponto de desenhar.

## 3. Inventário completo (o que o OpenGL faz hoje) e a etapa de cada item

Como o código do renderizador é o mesmo, "portar" um item quase sempre é: a `sgl` cobrir as
chamadas de GL dele + conferir o resultado contra o OpenGL. A etapa marca **quando é conferido**
(e quando a `sgl` precisa de algo específico para ele).

### 3.1 Interface com o emulador (`gpu_render.h`, tabela `GpuRenderBackend`) — (a)
Todas as entradas `gr_*`: init, scale, set_texture_filter, semi_transparency, mask_bits,
texture_window, color_modulation, precise_triangle, perspective_triangle, dither,
depth_triangle, fill_rect, copy_rect, flat/gouraud/textured/shaded-textured triangles,
flat/textured/textured-scaled rects, lines (flat/shaded), render_display(_hires),
vram_write/read/transfer_in/out, vram_upload_begin/commit/open, draw_area/offset, wide_*
(configure, set_view, set_target, disable_target, clear, clear_margins, render_wide_display,
wide_dump_full), hd_texture_note. `gr_backend()` continua `GR_BACKEND_OPENGL` com `sdl_gpu`
(o mesmo renderizador); quem precisa saber qual GL usa `gl_renderer_impl()`.

### 3.2 Modelo de VRAM e coerência — (a)
- Superfície autoritativa `s_hr_tex` RGBA8 1024·S × 512·S + depth/stencil D24S8 (bit 0 = bit de
  máscara; 0x02 = marca de brilho/bloom; 0x04 = 3D; 0x08 = "coberto"/translúcido por cima).
- Espelho nativo R16UI (`s_raw_tex`) para amostrar texturas (CLUT 4/8/15 bits no shader),
  passe PACK que regrava a região suja do hr no espelho, uniões sujas, lista exata de uploads
  pendentes (`s_up_rects`), staging que só cresce, textura de rascunho compartilhada.
- Leituras da CPU (`gl_renderer_sync_cpu`, `fbo_peek`, `fbo_peek_deferred`,
  `gr_vram_read/transfer_out`), modo depth24 (espelho da CPU autoritativo), restage depois de
  savestate, anel de coerência (`gl_coh_ring`), `vram_drawn_check` (blocos 16x16 desenhados).
- Janela de alta resolução (8K real em GPUs de 16384: blocos espelhados, fila diferida),
  limites de escala (`gl_scale_limits.h`; na `sgl` os limites vêm de `sgl_caps`).

### 3.3 Primitivas e regras do PS1 — (a)
Triângulos flat/gouraud/textura/textura sombreada, retângulos (o gpu.c manda quads 2D como
retângulo), linhas (`GL_LINES` em 1x; acima, quad de 1 px nativo; `glLineWidth` no espelho da
janela de alta resolução), fills (clear com scissor, com divisão nas bordas da VRAM), cópias
VRAM→VRAM (blit para rascunho + quad mascarado; divididas nas bordas como o hardware), uploads.
Semitransparência 4 modos (duas passadas por STP; duas fontes nos modos 0/1/3), máscara
set/check pelo stencil (GL_INVERT), janela de textura, `texture_window_batching`, dithering
(`[video] dithering`), `accurate_blending`, lotes LLE/HLE (`gl_batch_policy_*`, tornado
escolha em tempo de execução para o `NODUAL`), lotes de bancos imutáveis (`mod_texture_banks`),
`select_texture_bank(_live_clut)`.

### 3.4 Escala — (a), dinâmica em (g)
`internal_resolution`/`supersampling`, `request_internal_scale`, `step_internal_scale_now`,
teto (`set_internal_scale_ceiling`, menu Res), resolução dinâmica (passos, custo da render
thread, `dynres_stats`), `scale_info`, `anisotropy`, `texture_lod`.

### 3.5 PGXP / 3D Fix — (b)
Triângulo preciso (subpixel), textura em perspectiva, cor em perspectiva, profundidade por
triângulo (`pgxp_depth_buffer`, tolerância 0 = teste/gravação por triângulo, limiar), costura
(`pgxp_seam`), razões de limpeza (`pgxp_depth_clear_reasons`), `pgxp_render_stats`,
`draw_projected_triangle`, `projective_supported`.

### 3.6 Marcas, máscaras e capturas para os efeitos — (b)
Marca de bloom/brilho (`bcls`: fontes `additive`/`additive3d`, limiar), marca 3D (`b3d`, mask3d),
"coberto" (0x08), vidro (b3d 3: cópia da tela como textura), exclusão da UI na frente da OT
(`gpu_ot_front_now`, RTHF_FRONT), regra da poeira cinza (`uncovered`), `LIT_GREY_DIM`.
Captura da cena para sombras/luzes (`sc_add_*`): âncoras dos pés (sprite_anchors), peças de
sprite (agrupamento por CLUT/página, runs sólidas, limites), elipse/disco do jogo escondidos
(`shadow_blob_key`, `shadow_disc_quad`, "pokes" de CLUT RTH_VRAM_WRITE), entrada `sprite`
(RG32F, atlas, recorte da VRAM), peças aditivas paradas → luzes do fogo (aparência aprendida,
rampa), `scene_tone` (noite), `windows_casting`/`fire_casting`, imagens de fundo do 16:9
(`gpu_ws_set_backdrop_images`).

### 3.7 Mapa de sombra e sol/lua — (c)
Mundo (`gl_renderer_shadow_world`: grade de alturas do campo), memória de cenário (`s_scc`),
faces de costas (`shadow_add_backfaces`, `two_sided`), câmeras (`shadow_camera`,
`shadow_frame_camera`, `local_view_begin/end`), mapa dos sprites (`sprshadow`), sol por área
(`sun_areas`, `sun_area` TCP, ferramenta do sol, `gpu_shadow_light_*`), dia/noite por área
(`gpu_shadow_time_*`), `off_textures` (mapa-múndi), `set_night`, `shadow_cache` TCP.

### 3.8 Executor das cadeias `.pfx` — (c)
`gl_postfx.inc` + `postfx_chain.c`: `pfx 1`, `pass`, `shader`, `input` (source, previous,
source_down, mask, mask3d, depth, sprite, firemap, shadow, sprshadow, generated, nome de passe),
`lines`, `scale`, `format rgba8|rgba16f`, `filter linear|nearest`, `blend replace|additive`,
`reuse`, `run`, `uniform`; uniformes ao vivo (`set_post_uniform`, até 64), `clear_post_uniforms`,
`set_post_shader` (CRT/debug, por último), `set_extra_shader` slots 0..4 (AO, sol, luz,
cor/vinheta, downsample), preenchimento da profundidade (`postfx_note_fill`, modos 1/2,
blits de profundidade com escala e inversão), `u_time`, `u_proj`... Efeitos embutidos: bloom
(`bloom_bright`/`blur`/`composite`, `bloom_threshold`, `bloom_source`, `bloom_debug`), AA
(FXAA, FXAA HQ: `post_aa`, `post_aa_gpu_us`), gamma (`post_gamma`), scanlines embutidas.
Presets: `hd2d/ao`, `sun`, `sun-low`, `sun-high`, `light` (fogo, luzes colocadas, feixes),
`grade`, `downsample/box|adaptive`, `debug/depth|mask|shadowmap`, `debug_mask.pfx`, CRT do jogo
(`crt-geom-mini`, `crt-hyllian`, `crt-lottes`).

### 3.9 Luzes colocadas (`area_lights.c`) — (c)
`area_lights.txt`, amb/point/spot/window, u_placed/u_win[8]/u_al*, mapa cúbico do fogo (3x2 de
512 px, 4 lugares), feixes (`shaft`, 240 linhas), marcadores da ferramenta, TCP `area_lights`.

### 3.10 Widescreen nativo (16:9) — (d)
Superfícies largas por `base_x`, alvo/vista/deslocamento/margens, limpeza das margens, caminho
rápido (`set_wide_fast`), predicado por primitiva (DEFAULT/STRETCH/EDGES/KEEP), imagens de fundo
e sua memória (240 quadros), `nw_phase_backdrop`, `fit_wide_aspect`, `render_wide_display`,
`wide_dump_full`, `ws_ablate`, `note_wide_triangle_recovery`, `present_wide_fbo`, faixas
laterais (`pillarbox_fill`: borrado/cor lisa/preto, `set_pillarbox_blur`).

### 3.11 HDR (`gl_hdr.inc`) — (e)
Janela float16 (scRGB) — na `sgl`: swapchain `HDR_EXTENDED_LINEAR` (R16G16B16A16_FLOAT, o
mesmo scRGB) quando `SDL_WindowSupportsGPUSwapchainComposition`; último passo (contraste, luzes,
dinâmico, papel, pico), `hdr_set*`, `hdr_params`, `hdr_state`, `hdr_wanted`, `hdr_json`, TCP
`hdr` (`probe` com nits e foto falso-cor), linha HDR do menu.

### 3.12 Apresentação — (a) o básico, (f) o resto
Letterbox/aspecto (`set_display_aspect`), `present` (15 bits: blit do hr), `present_vram`,
depth24 (FMV: espelho da CPU em quad), `present_blank`, `present_hold_last`, `bezel`,
scanlines, `post_gamma`, intervalo de troca/VSync (`set_swap_interval`, presente modes do
SDL_GPU: VSYNC/IMMEDIATE/MAILBOX), ritmo (`present_pacing`, `frame_pacing.c`), anel de
apresentações (`present_ring`), sonda pós-load (`present_probe_*`), `invalidate_present`,
`present_rect_dirty`, **present thread** (contexto compartilhado; na `sgl`: desligado, a troca já
é barata — conferir em (g)), `window_to_display` (mouse), ponteiro do título
(`set_pointer*`), sobreposições do host em `gl_swap_with_osd` (OSD, volume, rewind, menu de
savestates, canvas/HUD/toast dos títulos), espelho de espectador.

### 3.13 Vídeos — (f)
24 bits (depth24), `fmv_enhance` (cor entre blocos, deblocking, FSR1 EASU+RCAS), `fmv_filter`,
`fmv_chroma_smoothing`, TCP `fmv_enhance`.

### 3.14 Filtros de textura — (f)
`texture_filtering`, classes Texture/Sprite/Menu (`set_texture_filter_class`), filtros 4..10 de
`gl_texfilter.inc` (Sharp bilinear, JINC2, cúbica monotônica, DCCI, Scale2x, Scale3x, MMPX) +
xBR, bordas PS1/No blend/Blend (Blend precisa de duas fontes → Clean no `sdl_gpu`), variantes
do programa (`TF_MASK`, `tex_prog_switch`), `texture_filter_stats`, TCP `texture_filter`.

### 3.15 Pacotes de textura HD — (f)
`gpu_hd_textures.cpp`, `hd_texture_pack.cpp`, `duckstation_texture_pack.cpp` (leitor do formato
de pacote, código nosso), `set_hd_texture_mode`, `hd_texture_cached`, `clear_hd_texture_cache`,
`texture_banks_supported`.

### 3.16 Render thread — (a) (mesmo código), sob carga em (g)
Gravação RTH_* e replay, fronteira de quadro, `render_thread_start/stop/sync/measure/costs/json`,
`render_thread_active`. Na `sgl`: a camada pertence à thread que estiver com o "contexto".

### 3.17 Geração e interpolação de quadros — (f)
`frame_generation` (redraw; reprojeção opcional), `frame_gen.c` (agenda, fase fixa), faixas e
preenchimento por pixel das bordas (`fgw_*`), `s_sc_held`, registro de envios (`vlog`),
`fg_effects_view`, cópia das marcas (stencil 1:1: ver 1.2), `frame_gen_*` (configure, hold,
counts, costs, json, prims_json, real_share, fg_source), `frame_interpolation.c`
(`set_interpolation*`, blend), `psx_video_set_smooth_motion`.

### 3.18 Fotos e leituras de imagem — (a) `present_shot`/`screenshot` (necessários aos testes), (f) o resto
`present_shot` (PNG do que se vê), `screenshot` (VRAM), `capture_display_hires`,
`read_display_hires`, `ring_capture`, anel de imagens apresentadas (PBO: `present_image_ring.c`),
fotos do relato de bug (Ctrl+B: foto com e sem efeitos, GIF), `psx_mod_request_screenshot`
(conquistas), captura F10.

### 3.19 Diagnóstico e TCP — (a) os que só leem estado, (g) os de tempo de GPU
`diag`, `runtime_diag`, `batch_diag`, `batch_sites`, `pass_*`, `coh_get`, `pres_get`,
`host_ledger`, `interpolation_diag`, `stereo_diag`, `frame_gpu_totals`, `gpu_totals`,
`postfx_gpu_totals`, `postfx_timing(_auto/_json)`, `perf_tex_gpu/_timing`, `perf_aggregate`,
`frame_perf` (consultas de tempo), `perf_meter.c`, `host_gpu_stats.c` (contadores do Windows,
independe do renderizador), comandos TCP `postfx_timing`, `frame_perf`, `shadow_cache`,
`frame_gen`, `render_thread`, `hdr`, `fmv_enhance`, `extra_shader`, `post_shader`,
`post_uniform`, `present_shot`, `screenshot`, `vram_drawn_check`, `lights`, `sprite_dump`,
`ws_backdrop_images`, `gl_coh_ring`, `present_ring`, `sun_area`, `area_lights`, `perf_frames`,
`perf_summary`, `texture_filter`, `bloom`. O TCP informa `renderer: "sdl_gpu"`.

### 3.20 Passes de render (`render_pass.c`) — (a) mesmo código, conferidos em (b)
`pass_begin/end/ready/diag/plan_diag/dump_arm/force_refuse/note_cost/service_presents/
set_flip_shown/verify_vram`.

### 3.21 Fora do BoF4 (ficam no OpenGL, documentado) — depois de (h)
Netplay (`cpu_auth_dual`, vistas locais, `local_viewport_renderer`), OpenXR (`psx_openxr.c`),
estéreo (`stereo_*`), espelho de espectador. Com `sdl_gpu` e um desses ligados: aviso e OpenGL.

### 3.22 Quem chama o renderizador de fora — (a)
`main.cpp` (161 chamadas `gl_renderer_*`; atributos de contexto, criação da janela com
`SDL_WINDOW_OPENGL`, `g_gl_active`, apresentação), `debug_server.c` (135), `gpu.c` (29),
`render_pass.c` (37), `gpu_hd_textures.cpp`, `perf_meter.c`, `bug_report.c`, `sun_tool`, e no jogo
`src/bof4_world.c`, `bof4_night.c`, `bof4_video_menu.c`, `bof4_bug_report.c` (~30 funções:
`set_post_uniform` 20×, `set_extra_shader`, `shadow_world/area`, `set_night`, `scene_tone`,
`hdr_*`, `set_bloom`, `set_pgxp_depth`, `set_post_aa`, `set_pillarbox_blur`,
`set_internal_scale_ceiling`, `set_dynamic_resolution`, `texture_filter_class`...). Como a API
`gl_renderer_*` é a mesma nos dois modos, **nenhuma dessas chamadas muda**; `main.cpp` só
ganha o caminho de janela/contexto do `sdl_gpu` (`g_gl_active` vale para os dois).

### 3.23 Outros usuários de OpenGL — (f)/(h)
`quality_probe.c` (abre um contexto GL para os presets de qualidade: no `sdl_gpu`, sonda pelo
dispositivo), `bug_report_ui.cpp` (Dear ImGui com o backend OpenGL3: compilado com o loader
apontando para a tabela `gl_api` → desenha pela `sgl`; precisa de `glDrawElements`/índices e
`glBufferSubData` na camada; até lá, o formulário antigo no canvas, que já existe para Vulkan),
o anexo de menus da recomp-ui (`attach(..., SDL_GL_GetCurrentContext())`, launcher dentro do
jogo: o BoF4 não usa, `skip_launcher = true`).

### 3.24 Configuração e launcher — (h)
`[video] renderer = "sdl_gpu"` (config_loader, settings.toml, CLI), `sdl_gpu_driver`, linha no
launcher do BoF4 (janela Game) e na página Display do jogo (troca exige reabrir), Launcher da
recomp-ui (`offer_vulkan` → oferecer `sdl_gpu`).

## 4. Etapas, cada uma com critério de pronto medido contra o OpenGL

Regra dos testes (dela): compilar e testes unitários leves (janela escondida, poucos quadros)
sem perguntar; **qualquer teste que abra o jogo, meça desempenho ou use a GPU pesado: perguntar
antes**. Jogo de teste só de `build-test/<pasta>` (RA desligado, saves próprios, `[memcard]`
reescrito, `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0`, janela 4x, mouse desligado, estado pelo
slot 10, porta ≠ 4370, sem tomar o foco).

Comparação pixel a pixel: o mesmo estado (`capture_states/*.pst`), o mesmo quadro do jogo
(contador VSync 0x80176514, como o `tools/perf_regress.py`), `present_shot` dos dois
renderizadores, diferença por pixel (`imgcmp.ps1`/`tools/scene_capture.py --compare`); ruído
aceito = o do baseline (cliff 0%, área 0,04%, batalha 0,11%, magia 13,9% — a magia varia entre
execuções do mesmo exe). Para o que não abre o jogo: os testes `runtime/tests/test_gl_*.c`
(incluem o próprio `gpu_gl_renderer.c` e comparam a VRAM com o renderizador de software) rodam
duas vezes, OpenGL e `sgl`, e as leituras dos dois têm de ser iguais.

### (a) Fundação + desenho base 4:3 + apresentação
1. `gl_api.h`: todo GL do renderizador pela tabela; com OpenGL, comportamento idêntico.
   Pronto: build-gpu compila; `test_gl_*` passam; (com permissão) 3 cenas iguais ao exe atual.
2. `sgl_caps` + fallbacks no renderizador (política `NODUAL` em tempo de execução, bordas Blend →
   Clean, tempos de GPU indisponíveis). Com OpenGL nada muda.
3. Camada `sgl`: estado, gravação/codificação, pipelines, samplers, formatos (RGBA8, R16UI,
   RGBA16F, RG32F, R32F, D24S8 / D32S8), FBOs, clears, blits, cópias, uploads, leituras (síncronas,
   PBO + fence), arena de vértices, linhas largas, uniformes, troca de tela.
4. glslang (aguarda a permissão do download) + cache de SPIR-V.
5. Janela/dispositivo no `main.cpp` (`renderer = "sdl_gpu"`, `--renderer sdl_gpu`, driver,
   VSync); falha → OpenGL → software.
Pronto: todos os `test_gl_*` com leitura idêntica nos dois; título, campo e batalha em 4:3 com
efeitos e 3D Fix desligados iguais ao OpenGL dentro do ruído.

### (b) Entradas extras: profundidade do 3D Fix, máscaras, camada dos sprites
Profundidade amostrável (textura D/S com SAMPLER), blits de profundidade por desenho, marcas do
stencil. Pronto: `debug/depth.pfx`, `debug/mask.pfx`, `debug_mask.pfx` e `debug/shadowmap.pfx`
iguais ao OpenGL nas cenas `cliff_campo`, `batalha`, `abertura_232`, `fogueira`; `sprite_dump`,
`shadow_cache` e `pgxp_render_stats` com os mesmos números.

### (c) Executor das cadeias `.pfx` (luzes e sombras)
Formatos float, até 8 entradas por passe, uniformes ao vivo, troca de shader ao vivo. Pronto:
AO, GI (sun/sun-low/sun-high), Spells/Fire, Night, luzes colocadas e feixes (despensa
0x8ba6e025), cor/vinheta, downsample, bloom e FXAA iguais ao OpenGL (cenas `fogueira`,
`quatro_pernas`, `abertura_232`, `barco_noite`, `usuaria_slot12`, `batalha`, a despensa).

### (d) Widescreen
Pronto: 16:9 nas cenas `ws_titulo`, `ws_load`, `ws_deserto`, `ws_mapa_cliff`, `abertura_324/328/336`,
`usuaria_slot1` (lua), batalha e acampamento iguais ao OpenGL, incluindo as faixas.

### (e) HDR
Pronto: TCP `hdr probe` com os mesmos nits e percentis do OpenGL (mesma cena); Windows com HDR
desligado = SDR igual.

### (f) Vídeos, pacotes, filtros, CRT, interpolação/geração de quadros, OSD
Pronto: abertura (FMV 24 bits com `fmv_enhance`), os 7 filtros nas 3 classes, os 3 CRT, Smooth
motion (quadros reais iguais; pisca medido como no OpenGL), menu de savestates, avisos,
conquistas, medidor, relato de bug (fotos e GIF), formulário ImGui, ponteiro do mouse.

### (g) Desempenho e turbo
Medir (com permissão; sempre com `PSX_FAST_FORWARD_SPEED=max`): CPU da thread de render por
quadro desenhado, GPU por quadro, turbo nas 8 cenas do `perf_walk`, tudo contra o OpenGL.
Otimizar a `sgl` (cache de pipeline sem hash repetido, ligações só quando mudam, menos quebras de
passe, anel de transferência mapeado, uniformes deduplicados, `SDL_SetGPUAllowedFramesInFlight`).
Turbo: hoje mostra 30 q/s e desenha todos os outros; conferir que o `sdl_gpu` também não espera a
tela (`PSX_TURBO_PRESENT_SKIP`). Tempos de GPU por fence para `postfx_timing`/medidor.
Pronto: thread de render ≤ a do OpenGL em todas as cenas e turbo ≥ o do OpenGL.

### (h) Launcher e menu
`renderer` no settings.toml, linha no launcher do BoF4, aviso no jogo; o OpenGL continua o
padrão. Pronto: trocar pelo launcher e abrir o jogo nos dois.

### (i) D3D12 e Metal; Linux/Deck
SPIRV-Cross → HLSL → `D3DCompile` (DXBC 5.1) e → MSL; build Linux (Deck: Vulkan nativo, sem
Proton). Pronto: as cenas de (a)–(d) iguais no D3D12; build Linux compila.

## 5. Riscos e como conferir

| # | Risco | Como conferir / contorno |
|---|---|---|
| R1 | Regra de preenchimento: GL e Vulkan desempatam pixels exatamente sobre uma aresta de jeitos diferentes (o mesmo mapeamento de linhas, mas "topo" invertido) → 1 px nas arestas diagonais compartilhadas | medir nas cenas de (a); se aparecer, renderizar invertido e desinverter na amostragem |
| R2 | Linha em 1x: rasterização de linha do Vulkan ≠ GL | medir; se preciso, linha 1x também vira quad |
| R3 | Profundidade (z+w)/2 no shader vs (z+1)/2 fixo do GL: 1 ulp pode trocar empates de LEQUAL | `debug/depth.pfx` e contagem de pixels; a tolerância 0 do PGXP é o caso a olhar |
| R4 | Sem duas fontes: mais desenhos no `NODUAL` (o dragão da ravina, 218 pares) | medir em (g); se pesar, lote ordenado por pixel (peças que não se cobrem dividem o desenho) |
| R5 | Quebras de passe por uploads no meio do quadro (GPUs de ladrilho: Mac/Deck pagam mais) | contador de passes por quadro (TCP); agrupar uploads |
| R6 | O SDL manda pegar o swapchain só na thread da janela; a render thread troca a tela | Windows/Linux Vulkan aceitam (testar); macOS: troca encaminhada à thread da janela |
| R7 | Compilação de shader/pipeline na 1ª vez (engasgos, como o `TF_MASK` de 7,9 s no GL) | cache de SPIR-V em disco; o driver guarda os pipelines; pré-compilar os programas do renderizador na abertura |
| R8 | Limites (16384) e memória em 8x | o renderizador já tem a janela de alta resolução; `sgl_caps` |
| R9 | HDR: composição scRGB nem sempre disponível no Vulkan (depende do driver/VK_EXT_swapchain_colorspace) | cair para D3D12 no HDR, ou SDR com aviso |
| R10 | Cópia só do stencil (geração de quadros) e leitura de profundidade exigem texturas D/S amostráveis | D24S8 com SAMPLER na NVIDIA; D32S8 onde não houver |
| R11 | Savestates e identidade de execução | a `sgl` não toca gte/pgxp/cpu_state; os saves valem nos dois |
| R12 | O que só o GL mede (consultas de tempo) | tempos por fence (g); até lá "unavailable" |

Cada etapa termina com commit no `feat/sdl-gpu-renderer` e um resumo para ela: o que está feito,
o que falta e o que precisa de teste (com permissão). Nada vai para o build-dev nem para o main
sem a aprovação dela; nada de push.

## 6. Diário

- 2026-10-31: worktrees criadas; inventário; plano. Pedida a permissão para baixar glslang e
  SPIRV-Cross (concedida: glslang 16.6.0, SPIRV-Cross vulkan-sdk-1.4.357.0, no deps.manifest).
- 2026-10-31, etapa (a) — fundação: `gl_api.h` (todo o GL do renderizador por uma tabela; com
  OpenGL nada muda), camada `sgl` (sgl_gl.c, sgl_gpu.c, sgl_shader.cpp), `[video] renderer =
  "sdl_gpu"` / `--renderer sdl_gpu`, TCP `sgl`. Achados: o SDL_GPU põe NDC y = +1 na linha 0 em
  todos os drivers (o epílogo do VS inverte y); o glslang numera varyings por ordem de
  declaração (a `sgl` numera pelo nome); um bloco de uniforms por estágio com nomes distintos.
  Conferido sem abrir o jogo (`runtime/tests/run_sgl_parity.py`, janela escondida): o mesmo
  teste nas duas implementações — `test_gl_readback_region` 331/340 checagens, 0 falhas em 1x e
  4x (VRAM palavra a palavra contra o renderizador de software); `texture_filter` (509),
  `post_aa`, `bloom` (146), `scale_invariance`, `fmv_chroma`, `fmv_enhance` passam nas duas;
  `blend_accuracy`, `dithering`, `texture_lod`, `xbr` falham igual nas duas (testes defasados:
  falham até no software), `render_thread`/`frame_gen` caem nas duas (stubs). Falta: o jogo
  (precisa da permissão dela), render thread e troca de tela fora da thread da janela (R6),
  cache de SPIR-V em disco, D3D12/Metal.
- 2026-10-31, no jogo (permissão dela; build-test/sgl_game, porta 4421): o sdl_gpu roda campo,
  masmorra, noite, fogueira, batalha, magia e menu. OpenGL × sdl_gpu (present_shot, mesmo
  estado e quadro; `PSX_PFX_TIME` congela o u_time — GL × GL dá 0%): sem efeitos e com 3D Fix,
  16:9 e Res 7, cliff/deserto/noite/menu idênticos; batalha 1% e fogueira 2% com 1–2 níveis
  (degradê da janela e o blend sem duas fontes); com os efeitos dela 0,01–3,7% dos pixels com
  1–3 níveis, nas arestas do cenário (profundidade arredondada diferente) e folhagem.
  Desempenho (perf_walk, 8x, turbo sem teto): no início, com efeitos, o sdl_gpu ia a metade
  do OpenGL (~4.400 desenhos e ~3.900 trocas de pipeline por quadro; o SDL grava descriptor
  sets a cada troca). Agora (3fe1983e: memória estado→pipeline, location→uniform, ligações só
  quando mudam, e peças semitransparentes que não se sobrepõem num lote só): campo 6,61× (GL
  6,63×), masmorra 2,82× (2,81×), batalha 6,73× (6,09×), magia 4,49× (4,37×), menu 9,75×
  (7,26×); sem efeitos igual. Os tempos de GPU dos efeitos não existem no sdl_gpu (R12).
- 2026-10-31, etapa (f) no jogo (permissão dela; build-test/sgl_game, porta 4421;
  `tools/local/sgl/stage_f.py` no repositório do jogo, fora do git). OpenGL × sdl_gpu:
  menu de pausa e de savestates iguais; filtros (os 7, nas 3 classes) iguais; CRT (os 3)
  iguais; vídeo de abertura com `fmv_enhance` igual (0,005–0,010%); ferramenta de Sol/Lua/Luzes
  (sun_area, area_lights pick/mesh/mark/force) com respostas idênticas e marcadores iguais.
  Corrigido no caminho:
  - Ctrl+B: o formulário ImGui agora desenha pela `sgl` (a tabela GL do ImGui, imgl3w, vem de
    `gl_renderer_get_proc`; `glDrawElements` reúne os vértices na ordem dos índices e desenha
    como arrays); 0,09% de diferença.
  - Bordas "Edge blending" sem duas fontes: passe de cor misturado pela cobertura + passe só do
    alpha (máscara); bate com o OpenGL (cliff de 0,71% para 0,000% acima de 8 níveis). Em peças
    semitransparentes segue Clean.
  - Turbo com pulo de desenho: uma apresentação atrasada mostrava o quadro pulado (preto); agora
    só se mostra quadro desenhado inteiro (0 de 227 fotos pretas; antes 6 de 72).
  - Consultas de tempo (R12) por fences: uma thread espera os fences em ordem e conta o tempo
    ocupado da GPU; o Smooth motion passou a planejar 2,7–2,9 gerados por real na batalha (antes
    1), como o OpenGL.
  - HDR: RGBA32F faltava (o tonemapping dinâmico nunca media: exposição presa em 0,85); agora
    luz, exposição e nits iguais ao OpenGL com o HDR do Windows ligado (branco 280, pico 1000).
  Vantagens do Vulkan (só no sdl_gpu):
  - HDR segue a tela ao vivo (scRGB com o Windows em HDR, swapchain SDR de 8 bits sem passe fora
    dele), saída HDR10 (PQ, BT.2020) onde o scRGB não existe (gamescope/Steam Deck: o HDR de lá
    é só Vulkan) ou com `PSX_HDR_FORMAT=hdr10`; HDR fora do Windows liberado no sdl_gpu. HDR10
    roda (188 trocas, sem avisos); as cores na tela ainda precisam do olho dela.
  - Smooth motion pela fila FIFO do swapchain (cada quadro real ou gerado com a sua atualização
    de tela; o GL do Windows não espera na troca). Batalha sem efeitos: trocas a ±25% da mediana
    35% → 89%. TCP `sgl` "acquire". `PSX_SGL_FG_FIFO=0` volta ao jeito do OpenGL.
  Falta: o olho dela no HDR10 e no ritmo; cache de shaders/pipelines (noite); (h) menu e launcher;
  (i) D3D12/Metal e Linux.
- 2026-11-01 (noite, autônoma): cache de SPIR-V em disco (`<exe>/cache/sgl_spirv`; 150 ms de
  glslang → 0 na 2ª vez; pior quadro ao entrar no campo 131 → 58 ms); pipelines ~0,15 ms cada
  (o driver guarda as suas), sem pré-criação. Também: AA só da geometria 3D (`hd2d/aa3d.pfx`,
  6º lugar de efeito), igual nos dois renderizadores. Relatório da noite no repositório do jogo
  (docs/NOITE_2026-11-01_RELATORIO.md).
- 2026-11-01 (manhã): trocas presas à fase da tela (`present_phase.c`, os dois renderizadores):
  numa tela múltipla do jogo (119,88 Hz) cada apresentação espera o meio da atualização certa
  (grade lida das atualizações reais por D3DKMT) e troca com intervalo 1; o pacer é puxado de leve
  (≤ 0,3% de um período) para manter a fase. O quadro vai à GPU antes da espera (o sdl_gpu só
  submetia na troca). Andando no deserto (OpenGL): imagens fora das 4 atualizações 183/1072 →
  9/1070; sdl_gpu: trocas 32,9–33,8 ms. TCP `present_phase`; `PSX_PRESENT_PHASE=0` desliga,
  `PSX_PRESENT_PHASE_PROBE=1` mede em qual atualização cada imagem cai (GL_TIMESTAMP).
