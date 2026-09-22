# PanVK G57 / WinlatorMali — primeira build Android candidata

Esta configuração prepara um driver Android ARM64 para ser carregado sob o wrapper do WinlatorMali via AdrenoTools. Não é um wrapper novo, não é uma correção do bug de textura e ainda não foi compilada no GitHub Actions nem executada no telefone.

## Base e alterações preservadas

Base Mesa: `0175a4fe02beb8e58357ebb41eb13c86743b59d0` de `mexicanbr0auth/mesa-panvk-g57`.
WinlatorMali estudado: branch `bionic-mali-1.0`, commit `fbf42d26411444249a301086da0dcb652b861b17`.
O APK instalado pode corresponder a outro estado do código.

O pacote local fornecido substitui apenas:

- `src/panfrost/vulkan/jm/panvk_vX_gpu_queue_kbase.c`
- `src/panfrost/vulkan/jm/panvk_vX_cmd_draw.c`

`pan_kmod.c` é idêntico à base. A ponte HAL, o meson.build de PanVK, panvk_android.c e kbase_kmod.c também coincidem com os hashes locais fornecidos. Isso NÃO comprova identidade de toda a árvore local.

Diferenças revisadas: o arquivo de draw desativa mensagens via PANVK_PERF_NOLOG. O arquivo de queue desativa/remove dumps caros, preserva o reempacotamento de VERTEX_ARRAY em j[34], conserva estatísticas JM_WAIT/JM_SUBMIT_DIST e inclui o experimento opcional PANVK_JM_PIPELINE. Esse experimento fica desligado se a variável estiver ausente; não o ative para validar esta build. Os arquivos importados são preservados byte a byte e verificados por SHA-256.

## Decisões de compilação

- Runner Ubuntu 24.04 x86_64; LLVM/Clang 19 para ferramentas que executam no runner.
- NDK 28.2.13676358; alvo aarch64-linux-android35.
- Mesa native tools da MESMA fonte: mesa_clc, vtn_bindgen2, panfrost_compile.
- SPIRV-Tools/Headers vulkan-sdk-1.4.309.0 para host, com objetos de tag verificados.
- Mesa target: platforms=android; kbase e panthor preservados; libdrm estático.
- panfrost-rust=false: evita introduzir a toolchain Kraid/Rust nesta primeira candidata G57. Isso é uma diferença de configuração, não um patch de renderização.
- LLVM e SPIRV-Tools não entram como bibliotecas de runtime no alvo Android.
- Sem X11/XCB, shader disk cache, zlib/zstd, expat ou caminho privado do Termux no driver alvo. O wrapper existente mantém a interface de apresentação do Winlator.
- C++ runtime estático; o empacotamento rejeita libc++_shared.so e dependências inesperadas.
- android-stub=true usa os placeholders de ABI da própria árvore Mesa SOMENTE para linkedição cruzada. Nenhuma dessas bibliotecas stub entra no ZIP. O driver usa bibliotecas reais do Android no aparelho; falhas de namespace/símbolo devem ser corrigidas, não mascaradas com stubs.
- Os pacotes APT do runner não estão presos a versões exatas: as versões relevantes e commits são registrados. Não há promessa de build bit a bit reproduzível.

## Executar no GitHub

O workflow `.github/workflows/build-winlatormali.yml` roda por push na branch `winlatormali-ci`. Também declara workflow_dispatch; o botão manual normalmente exige que a definição do workflow esteja na branch padrão. Para a primeira execução use o push na branch separada.

Não sobrescreve main, não modifica builds Termux e não publica releases. A permissão do workflow é somente contents:read. Gera artefatos de candidato e logs.

Se falhar, baixe `PanVK-G57-WinlatorMali-build-logs`. Se compilar, baixe o artefato `PanVK-G57-WinlatorMali-candidate` e extraia o ZIP externo do Actions. O ZIP interno `PanVK-G57-WinlatorMali-TEST-<commit>.zip` é o pacote AdrenoTools (meta.json + .so).

Não importe o ZIP de preparação deste documento como driver.

## Validação no aparelho

1. Preserve os drivers anteriores e use um container de teste.
2. No código auditado existe a opção para mostrar o menu AdrenoTools em GPUs não Adreno. A presença dessa opção no APK precisa ser conferida.
3. Importe o ZIP interno no gerenciador AdrenoTools e selecione esse driver sob o wrapper. Não substitua wrapper_icd.aarch64.json pelo ICD do Termux.
4. Primeiro confirme carregamento sem erros de biblioteca/símbolo e enumeração de PanVK. Nome da GPU sozinho não comprova qual biblioteca foi carregada; o wrapper pode alterar as propriedades exibidas.
5. Depois teste criação de device e swapchain/apresentação. O caminho AHB/gralloc -> FD DMA-BUF -> kbase existe na fonte, mas precisa ser validado no aparelho.
6. Só depois avalie jogos. A corrupção conhecida do GFX Buffer->Image ainda existe nesta base.

Não force versões Vulkan/extensões para contornar erros de inicialização. Não copie as bibliotecas de `src/android_stub` para o dispositivo. Nomes admitidos na lista de dependências Android não garantem sua acessibilidade no namespace usado por AdrenoTools.

## Verificações realizadas antes da entrega

- Leitura e comparação das fontes locais com a base Git.
- Sintaxe Bash e Python; estrutura YAML e triggers.
- Verificação dos objetos de tag SPIRV no GitHub.
- Aplicação do patch em índice temporário da base, sem alterar a árvore existente.
- Não realizada: compilação Linux/NDK completa, execução do Actions, teste Vulkan/G57.
