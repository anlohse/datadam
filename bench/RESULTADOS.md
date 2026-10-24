# Benchmark: datadam × Redis × Kvrocks (memtier_benchmark)

Segunda rodada, de 22/10/2026, feita depois da investigação da cauda de latência descrita abaixo. Os dados
brutos (JSON e saída do memtier, uso de memória e disco) estão em `bench/results/`:

| cenário | clientes independentes (`distinct`) | sementes iguais (`same`) |
|---|---|---|
| `big`, dados 3× a memória | `big-distinct-20261022-192201/` | `big-same-20261022-193940/` |
| `fit`, dados na memória | `fit-distinct-20261022-195827/` | `fit-same-20261022-201743/` (só datadam) |

A primeira rodada (`big-20261022-163520/` e `fit-20261022-161636/`) está resumida no fim. Para repetir:

```bash
SEEDS=distinct bash bench/memtier.sh big       # ou fit; SEEDS=same reproduz o padrão do memtier
python bench/report.py bench/results/<cenário>-<sementes>-<data>
```

## Ambiente e método

- **Máquina.** Docker Desktop no Windows 11, com uma VM de 28 vCPUs e 7,8 GB de RAM; disco virtual sobre SSD.
  Cada servidor roda sozinho num contêiner, com limite rígido de memória e sem swap, sobre um volume novo. O
  memtier roda em outro contêiner na mesma máquina e divide a CPU com o servidor.
- **Versões.** memtier_benchmark 2.5.2, Redis 7.4, Kvrocks 2.17.0 e datadam 0.1.0.
- **Configuração.** Os orçamentos de memória são equivalentes: cerca de metade do limite do contêiner em caches,
  e o resto para memtables, conexões e o próprio processo.

  | | `fit` (limite 2 GB) | `big` (limite 1 GB) |
  |---|---|---|
  | datadam | pool de objetos 1 GB + block cache 256 MB, 8 threads de I/O | pool 384 MB + block cache 128 MB, write buffer 32 MB, 8 threads |
  | Kvrocks | block cache 1280 MB, 8 workers (padrão) | block cache 512 MB, write buffer 32 MB, 8 workers |
  | Redis | maxmemory 1800 MB, noeviction, 4 threads de I/O | maxmemory 900 MB, noeviction, 4 threads de I/O |

  O datadam roda com o read pool padrão, de 64 threads. Os três gravam um log a cada escrita e fazem fsync uma
  vez por segundo: o WAL do datadam e do Kvrocks e o AOF `everysec` do Redis. O Redis usa `noeviction` porque
  um banco não pode descartar dados.
- **Dados.** 512 bytes por chave, com valores **realmente aleatórios**. O `--random-data` do memtier reaproveita
  o mesmo bloco aleatório em todas as chaves: mil valores dele comprimem 89× com gzip. Num banco que comprime
  (RocksDB com LZ4), um conjunto de "3 GB" assim ocupa poucas centenas de MB e cabe na memória. Por isso:
  - os dados são carregados por `bench/load_random.py`, com `os.urandom` por chave e os mesmos nomes de chave
    do memtier (`memtier-1` … `memtier-N`);
  - o memtier roda as cargas de trabalho por cima desses dados;
  - como as escritas do memtier voltam a ser compressíveis, cada carga que escreve começa de um conjunto de
    dados recém-carregado.
- **Sementes dos clientes.** Por padrão, o memtier dá a mesma semente aleatória a todas as conexões. As 100
  conexões pedem então a mesma sequência de chaves, quase ao mesmo tempo. A primeira busca a chave no disco e
  as outras 99 a encontram no cache. Isso transforma dados 3× maiores que a memória num teste de cache quente.
  `--distinct-client-seed` dá uma sequência própria a cada conexão, que é o caso real. A rodada principal usa
  `distinct`; a `same` está aqui porque é o que o memtier faz quando ninguém pede outra coisa, e porque mostra
  o comportamento com uma chave pedida por muitos clientes ao mesmo tempo.
- **Cargas de trabalho.** 60 s cada, com 100 conexões (4 threads × 25 clientes) e sem pipeline, exceto onde
  indicado.
  - `read-uniform`: 100% `GET`, chaves uniformes.
  - `read-gaussian`: 100% `GET` com distribuição gaussiana sobre as chaves.
  - `mixed-gaussian`: 20% `SET` e 80% `GET`, gaussiano.
  - `mixed-gaussian-p16`: o mesmo, com pipeline de 16.
  - `write-uniform`: 100% `SET`, chaves uniformes, sobrescrevendo.
- **Cenários.**
  - `fit`: 1 milhão de chaves (cerca de 0,5 GB de valores), contêineres de 2 GB. Tudo cabe na memória.
  - `big`: 6 milhões de chaves (cerca de 3 GB de valores), contêineres de 1 GB. Os dados são **3× maiores que
    a memória**, e o page cache também conta dentro do limite.
- **Compare só dentro da mesma rodada.** O disco do contêiner é um arquivo no Windows, que o host guarda em
  cache na própria RAM quando sobra memória. A velocidade do "disco" muda com o resto da máquina. O mesmo
  Kvrocks fez 212 mil `GET`/s no `big` da primeira rodada e 73 mil/s nesta, com a mesma configuração do
  memtier. Por isso os servidores são comparados entre si dentro de cada rodada, e o antes/depois do datadam
  foi medido com execuções alternadas (seção da investigação).

## Cenário `big`: os dados têm 3× o tamanho da memória

Clientes independentes (`distinct`, o caso real):

| carga (ops/s, latência p50 / p99 / p99,9 em ms) | datadam | Kvrocks |
|---|---|---|
| read-uniform | **54.056** · 1,61 / 6,62 / 9,60 | 33.545 · 2,80 / 10,75 / 16,51 |
| read-gaussian | **67.669** · 1,48 / 3,10 / 4,45 | 35.967 · 2,65 / 9,66 / 14,65 |
| mixed-gaussian | **62.467** · 1,35 / 6,11 / 9,60 | 38.382 · 2,16 / 12,61 / 24,83 |
| mixed-gaussian-p16 | **79.319** · 15,81 / 76,80 / 112,64 | 45.157 · 33,28 / 134,14 / 198,66 |
| write-uniform | **134.812** · 0,43 / 6,14 / 28,03 | 45.638 · 0,39 / 1,61 / 421,89 |

Sementes iguais (`same`, o padrão do memtier):

| carga | datadam | Kvrocks |
|---|---|---|
| read-uniform | 62.177 · 1,41 / 6,30 / 12,03 | **72.751** · 1,33 / 5,31 / 9,66 |
| read-gaussian | 84.429 · 1,34 / 2,53 / 3,74 | **88.530** · 1,30 / 3,18 / 3,77 |
| mixed-gaussian | 66.640 · 1,30 / 5,98 / 12,03 | **93.016** · 1,26 / 3,41 / 4,80 |
| mixed-gaussian-p16 | **408.342** · 3,26 / 13,76 / 43,26 | 113.918 · 13,44 / 30,46 / 67,07 |
| write-uniform | 199.026 · 0,38 / 4,93 / 8,32 | **208.536** · 0,29 / 0,79 / 2,70 |

| `distinct` | datadam | Kvrocks |
|---|---|---|
| carga de 6 M chaves | 27,5 s | 49,6 s |
| memória após a carga | 549 MiB | 159 MiB |
| disco após a carga | 5.760 MB | 6.356 MB |
| disco ao final | 10.878 MB | 12.053 MB |
| processo morto por falta de memória | não | não |

O Redis não entrou de novo no `big`: com `noeviction` ele guarda só as primeiras 1,33 milhão de chaves (22%) e
recusa as escritas seguintes, como mostrou a primeira rodada.

## Cenário `fit`: os dados cabem na memória

Clientes independentes (`distinct`):

| carga | datadam | Kvrocks | Redis |
|---|---|---|---|
| read-uniform | **490.755** · 0,17 / 0,49 / 0,65 | 475.544 · 0,19 / 0,57 / 0,79 | 341.654 · 0,29 / 0,66 / 1,08 |
| read-gaussian | **476.882** · 0,18 / 0,51 / 0,66 | 454.166 · 0,20 / 0,60 / 0,80 | 342.820 · 0,30 / 0,62 / 1,00 |
| mixed-gaussian | **425.127** · 0,20 / 0,57 / 0,81 | 282.481 · 0,21 / 2,16 / 18,30 | 278.182 · 0,36 / 0,78 / 1,41 |
| mixed-gaussian-p16 | **1.127.578** · 1,14 / 10,75 / 13,25 | 265.153 · 3,57 / 58,37 / 164,86 | 593.516 · 2,37 / 7,42 / 12,35 |
| write-uniform | 198.633 · 0,36 / 3,60 / 4,86 | 135.752 · 0,61 / 2,99 / 6,43 | **205.887** · 0,46 / 1,23 / 10,62 |

| `distinct` | datadam | Kvrocks | Redis |
|---|---|---|---|
| carga de 1 M chaves | 3,8 s | 5,1 s | 3,2 s |
| memória após a carga | 926 MiB | 94 MiB | 688 MiB |
| disco após a carga | 551 MB | 1.077 MB | 508 MB |

Com sementes iguais (`same`), o datadam fez 470.512 / 457.185 / 426.885 / **1.596.645** / 281.706 ops/s nas
cinco cargas (p99,9 de 0,75 / 0,76 / 0,82 / 3,36 / 1,41 ms). Na primeira rodada, antes das mudanças, tinham
sido 486.400 / 481.709 / 443.663 / 1.694.585 / 298.455. A diferença de 3 a 6% está dentro da variação do
ambiente, então as mudanças não pioraram o caso em que tudo cabe na memória.

## A cauda da carga mista com pipeline (dados 3× a memória)

Na primeira rodada, a carga `mixed-gaussian-p16` do cenário `big` deu ao datadam um p99,9 de 102 ms, contra
37 ms do Kvrocks. A investigação mediu no servidor, com histogramas de latência (hoje em `INFO latency`), onde
o tempo ia, e encontrou três causas encadeadas.

1. **Locks de batch presos durante leituras de disco.** Um pipeline segurava os locks das chaves que escrevia
   até o fim do pipeline. Os `GET` que erravam o cache também prendiam as suas chaves, e liam o disco segurando
   tudo isso. Outro cliente que precisava de uma dessas chaves esperava, bloqueando uma das 8 threads de I/O.
   Medido: 100 mil esperas em 30 s, com mediana de 1,3 ms e p99,9 de 24 ms; os commits no RocksDB, em
   contrapartida, levavam 6 µs. **Correção:** leituras não entram mais no batch (travam a chave só durante a
   leitura), e o batch é gravado antes de qualquer leitura de disco. O tempo em que um batch segura locks
   caiu de 7 ms para 0,2 ms no p99,9, e os conflitos de 61 mil para 800.
2. **A mesma chave pedida por todos ao mesmo tempo.** Com as sementes iguais do memtier, 99,98% das esperas
   restantes eram pela *mesma chave* que outro cliente estava lendo do disco. Cada espera bloqueava uma thread
   de I/O, e com 8 threads o servidor inteiro parava a cada leitura de disco. O filtro de admissão do cache
   piorava isso, porque recusava a chave no primeiro acesso e forçava uma segunda leitura de disco. O mesmo
   acontece de verdade quando uma chave quente sai do cache (*cache stampede*).
3. **Só 8 leituras de disco simultâneas.** Com clientes independentes, quase todo `GET` vai ao disco, e cada
   leitura ocupava uma thread de I/O. Com 8 threads, o servidor fazia de 16 a 25 mil ops/s. Com 32 threads de
   I/O, o mesmo teste foi a 76 mil, o que confirmou a causa.

A correção de 2 e 3 é o **read pool** (descrito no README, em "Leituras que vão ao disco"). Uma leitura pura
que erra o cache sai da thread de I/O e espera de forma assíncrona. Um pool de threads lê a chave uma única vez
para todos que a pedem, e as chaves do resto do pipeline são carregadas em paralelo.

Antes e depois, `mixed-gaussian-p16` no `big`, 30 s por execução, alternando as versões:

| sementes | versão | ops/s | p99 | p99,9 |
|---|---|---|---|---|
| iguais | antes | 267.926 / 245.888 | 23,9 / 35,6 | 38,1 / 59,6 |
| iguais | locks corrigidos; leitura bloqueada tenta de novo por timer (sem pool) | 304.980 / 302.163 | 22,3 / 24,4 | 32,5 / 45,8 |
| iguais | **com read pool** | **575.507 / 569.065** | **6,7 / 6,6** | **17,8 / 19,3** |
| independentes | antes | 25.300 / 16.073 | 309 / 332 | 403 / 377 |
| independentes | **com read pool** (32 threads) | **66.251 / 72.351** | **101 / 97** | **148 / 126** |
| independentes | read pool com 64 / 128 threads | 81.885 / 88.766 | — | 84 / 75 |

Sem pipeline, com clientes independentes e `read-uniform`, o read pool fez 32.793 ops/s (p99,9 de 22,5 ms),
contra 8.432 ops/s (p99,9 de 97 ms) do mesmo binário com `read-threads 0`. Com sementes iguais e sem
pipeline, as versões ficaram dentro do ruído (antes: 64 e 43 mil; com pool: 61 e 51 mil; sem pool: 63 e
69 mil). Nesse caso todos os clientes andam atrás de um só, e a passagem pelo pool soma alguns microssegundos
ao caminho dele.

Também foi testado um **filtro de admissão** no cache (TinyLFU), com a hipótese de que leituras avulsas
expulsavam as chaves quentes. Nesta carga ele não mudou a taxa de acerto do cache, de 96% com ou sem ele.
O filtro ficou, porque evita que uma carga em massa troque o cache inteiro, mas não era a causa da cauda.

## Leitura dos resultados

1. **Dados maiores que a memória, clientes independentes: o datadam lidera todas as cargas.** Ele faz de
   1,6× a 1,9× o Kvrocks nas leituras e na carga mista, e 3× na escrita pura, com p99,9 menor em todas.
   Na carga mista com pipeline, o p99,9 é de 113 ms contra 199 ms do Kvrocks, e a vazão é 1,8× maior.
2. **Com sementes iguais, o Kvrocks ganha as cargas sem pipeline.** A diferença é de 5% nas leituras
   gaussianas e na escrita pura, de 17% nas leituras uniformes e de 40% na mista. Nesse teste o memtier faz os
   100 clientes andarem atrás de um só, que vai ao disco, e cada leitura desse cliente passa pelo read pool.
   Na mista, os 100 clientes também escrevem a mesma chave quase juntos e disputam o lock dela. Com pipeline,
   o datadam faz 3,6× o Kvrocks.
3. **Com os dados na memória, o datadam lidera tudo, exceto a escrita pura.** Na escrita pura o Redis faz 4%
   a mais. Com pipeline, o datadam faz 1,13 M ops/s, contra 594 mil do Redis e 265 mil do Kvrocks. O preço é
   memória: o datadam usa o pool que recebeu (926 MiB, contra 688 MiB do Redis).
4. **O Redis não funciona como banco com dados maiores que a memória.** Configurado para não descartar dados
   (`noeviction`), ele passa a recusar escritas.
5. **Disco.** Ao fim do cenário `big` com clientes independentes, o datadam ocupava 10,9 GB e o Kvrocks 12,1 GB,
   para 3,1 GB de valores. As escritas espalhadas por 6 milhões de chaves deixam a compactação do RocksDB atrás
   nos dois. O Kvrocks também retém o WAL por 3 h por padrão (`rocksdb.wal_ttl_seconds 10800`).
6. **O que ainda limita o datadam no `big`.** É o próprio disco: o p50 de uma leitura no RocksDB foi de
   cerca de 450 µs, e na carga com pipeline a fila do read pool chega a 7 ms. Ler as chaves em lote com
   `MultiGet` não ajudou (próxima seção).

## Lotes com `MultiGet` no read pool

O read pool ganhou a opção `read-batch`. Quando há mais chaves na fila do que threads, cada thread pega até
`read-batch` chaves e lê todas com um único `MultiGet` do RocksDB. A medição usou `mixed-gaussian-p16` no `big`,
com clientes independentes, 30 s por execução e as configurações alternadas na mesma janela.

| configuração | ops/s nas 5 execuções | média | p99,9 (mediana) |
|---|---|---|---|
| `read-batch 1` (uma leitura por chave) | 39.425 / 71.282 / 51.680 / 89.149 / 90.785 | 68.464 | 110 ms |
| `read-batch 16` | 92.903 / 79.377 / 53.753 / 63.972 / 68.590 | 71.719 | 110 ms |
| `read-batch 16` com io_uring (`seccomp=unconfined`) | 72.229 / 51.616 | 61.923 | 128 ms |

A diferença fica dentro do ruído. Os histogramas mostram o porquê: um lote médio de 2,5 a 2,8 chaves levou
3 a 3,5 ms no p50, enquanto um `Get` levava 0,45 ms. O RocksDB lê em paralelo, com io_uring, só os blocos de um
mesmo arquivo SST. Arquivos diferentes ele lê um depois do outro, a não ser que tenha sido compilado com as
corrotinas do folly. Chaves aleatórias em 3,6 GB quase nunca caem no mesmo arquivo, então o lote troca
leituras paralelas (uma por thread) por leituras em sequência e economiza só CPU. O código ficou, coberto por
testes, para cargas com chaves próximas; o padrão é `read-batch 1`, e o build não usa mais o liburing. Ler o
disco em paralelo de verdade exigiria leitura assíncrona entre arquivos.

## Primeira rodada (antes das mudanças)

Feita no mesmo dia, com as sementes iguais do memtier, antes da investigação acima. O "disco" estava bem mais
rápido nessa hora (veja o método), então os números absolutos não se comparam com os de cima.

| `big` (ops/s, p50 / p99 / p99,9 ms) | datadam | Kvrocks | Redis |
|---|---|---|---|
| read-uniform | 233.541 · 0,38 / 2,00 / 3,44 | 212.432 · 0,48 / 1,52 / 2,17 | só 22% das chaves existem |
| read-gaussian | 264.505 · 0,38 / 1,43 / 2,13 | 224.812 · 0,45 / 1,66 / 2,22 | só 22% das chaves existem |
| mixed-gaussian | 214.263 · 0,38 / 2,19 / 4,38 | 230.515 · 0,37 / 1,98 / 3,07 | 100% dos `SET` recusados |
| mixed-gaussian-p16 | 250.040 · 3,92 / 28,93 / 102,40 | 324.911 · 3,58 / 21,12 / 37,38 | 100% dos `SET` recusados |
| write-uniform | 285.367 · 0,34 / 0,68 / 3,85 | 262.971 · 0,30 / 0,78 / 2,06 | 100% dos `SET` recusados |

No `fit` dessa rodada, o datadam liderou as cinco cargas: 1,69 M ops/s com pipeline, contra 1,10 M do Redis
e 0,87 M do Kvrocks. O Redis guardou só 1,33 M das 6 M chaves no `big` e recusou 4,67 M.

## Limitações deste benchmark

- O memtier, o carregador e o servidor dividem as mesmas 28 vCPUs. Numa rede de verdade, a latência teria a
  rede somada e a CPU do cliente não competiria com a do servidor.
- O disco é o disco virtual do Docker Desktop sobre um SSD do Windows, com o cache e o antivírus do host no
  meio, e a velocidade dele muda com a memória livre do host. Em NVMe dedicado, as cargas que leem do disco
  devem melhorar para datadam e Kvrocks.
- Cada carga roda 60 s, uma vez por rodada. Execuções repetidas da mesma carga variaram de 10% a 40% neste
  ambiente; diferenças menores que isso não são conclusivas.
- O memtier 2.5.2 registra em quase toda execução, para todos os servidores, uma amostra de cerca de 604 s
  (`Max Latency: 603979.775`), o que infla a latência média. As tabelas usam só os percentis.
- Só strings (`GET`/`SET`). Coleções e índices secundários têm benchmarks próprios no README.
