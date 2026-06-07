# Diario tecnico

Questo file raccoglie lo stato operativo del progetto e andra aggiornato durante lo sviluppo. Non sostituisce il testo ufficiale: la fonte di verita resta `docs/Testo.md`, limitatamente al progetto base.

## 2026-06-07

### Avanzamento

- Nelle ultime commit è stata aggiunta la prima implementazione interna del processo Mapper:
  - `mapper_emit_pair` valida i token alfanumerici ASCII e serializza coppie `<token, processed_token>` sulla pipe;
  - il valore `processed_token` viene trattato come byte opaco con lunghezza esplicita, anche quando contiene byte nulli;
  - il thread lettore del mapper legge record di riga da `STDIN_FILENO`, li inserisce nella coda condivisa e chiude la coda su EOF;
  - il worker mapper estrae righe dalla coda, costruisce la vista pubblica `mr_file_line_t`, invoca la callback applicativa e libera il record proprietario consumato.
- Sono stati aggiunti test in `tests/test_io.c` per:
  - verificare il formato binario prodotto da `mapper_emit_pair`;
  - verificare l'accettazione di valori opachi con byte nullo interno;
  - verificare il rifiuto di token non validi e di `value == NULL` con dimensione positiva;
  - verificare il percorso `line_queue_push` -> `mapper_worker_main` -> callback mapper -> `mapper_emit_pair`.

### Scelte tecniche

- La scrittura delle coppie del mapper resta protetta da un mutex C11 dedicato, perché in seguito più worker potranno emettere sulla stessa pipe. L'alternativa era lasciare scritture non sincronizzate contando sulla dimensione ridotta dei record, ma non sarebbe una garanzia sufficiente sul formato logico.
- I test del worker chiamano `mapper_worker_main` direttamente dopo aver preparato una coda già popolata e chiusa. L'alternativa era creare veri thread nel test, ma per ora il comportamento importante è il contratto del worker, non lo scheduler.
- Il test controlla il record di coppia leggendo header, token e valore dalla pipe, invece di interpretare il payload come stringa. Questo mantiene fermo il requisito del testo: `processed_token` è una sequenza di byte opaca.

### Verifiche

- È stato eseguito `gcc -Iinclude -Wall -Wextra -pedantic -std=c11 tests/test_io.c -o /tmp/mr-test_io` sulla macchina host.
- La compilazione host non è arrivata ai test perché manca `<threads.h>`, coerentemente con il fatto che il progetto va compilato nel dev container Ubuntu 24.04.
- Non è stato eseguito `make` sulla macchina host.

### Prossimi passi

1. Eseguire nel dev container la compilazione diretta di `tests/test_io.c` e il binario prodotto.
2. Se i test passano, collegare il processo mapper al flusso di `mr_start`: pipe, fork, `dup2`, chiusura descrittori e `waitpid`.
3. Prima di passare al reducer, definire il punto esatto in cui il processo mapper chiude lo standard output dopo la terminazione dei worker.

### Punti da saper spiegare

- Perché il token viene validato come stringa C alfanumerica, mentre il valore resta byte opaco.
- Perché le emissioni dei worker verso la stessa pipe devono essere sincronizzate.
- Come passa l'ownership di una riga dalla coda al worker e perché il worker la distrugge dopo la callback.
- Perché EOF sulla pipe delle righe viene tradotto in chiusura della coda interna del mapper.

## 2026-06-05

### Avanzamento

- È stato implementato il riconoscimento di `input_path` come file regolare o directory tramite una funzione interna `write_input_path_lines`.
- Nel caso di file regolare, viene riusata `write_file_lines` e il nome logico passato al mapper è il basename del path.
- Nel caso di directory, vengono raccolti solo i file regolari contenuti direttamente nella directory, senza scansione ricorsiva.
- I file della directory vengono ordinati lessicograficamente per `file_name` prima della serializzazione delle righe, come richiesto dal testo.
- Sono stati aggiunti test I/O per file singolo, directory ordinata, sottodirectory ignorata e tipo di input non supportato.

### Scelte tecniche

- È stata introdotta una struttura interna `mr_input_file_t` con `full_path` e `file_name`, per distinguere il path fisico usato dal framework dal nome logico visibile al mapper.
- Per la directory è stato scelto un array dinamico con crescita geometrica tramite `realloc`. L'alternativa era fare una prima scansione per contare i file e una seconda per riempire un array allocato una sola volta; per ora si evita la doppia scansione e si mantiene il codice più diretto.
- Le stringhe salvate nell'array vengono duplicate con `strdup`, perché `full_path` è temporaneo e `entry->d_name` appartiene alla struttura gestita da `readdir`.
- L'ordinamento usa `qsort` e confronta `file_name`, non `full_path`, perché il testo richiede ordine lessicografico rispetto al nome del file nella directory.
- Per ora viene usato `stat`: eventuali symlink verso file regolari vengono trattati come file regolari. Se si decidesse di escluderli, andrebbe usato `lstat`.

### Verifiche

- È stato eseguito `git diff --check` con esito positivo.
- Non è stato eseguito `make` sulla macchina host.
- I test sono stati eseguiti nel dev container dal programmatore e sono passati.

### Prossimi passi

1. Integrare `write_input_path_lines` nel flusso di `mr_start` quando verrà costruita la pipeline.
2. Chiarire definitivamente l'ownership di `mr_line_item_t` prima di collegare la coda del mapper.

### Punti da saper spiegare

- Differenza tra path fisico e nome logico del file.
- Perché `entry->d_name` non basta per aprire i file dentro una directory.
- Perché la directory va ordinata dopo la raccolta dei file regolari.
- Perché `mr_input_file_t **files` serve quando si usa `realloc`.
- Quale overflow viene evitato con `SIZE_MAX / sizeof(**files)`.

## 2026-06-04

### Avanzamento

- È stata implementata la lettura di un singolo file regolare tramite una funzione interna che usa `fopen` e `getline`.
- Ogni riga letta viene trasformata in un `mr_line_item_t` e serializzata sulla pipe tramite `write_line_record`.
- Il carattere `'\n'` finale non viene incluso nella riga logica passata al mapper.
- Sono gestiti i casi richiesti dal testo per il file singolo: file vuoto, righe vuote, righe normali e ultima riga non terminata da `'\n'`.
- I test I/O coprono il collegamento tra lettura da file e serializzazione dei record di riga.

### Scelte tecniche

- Per la lettura delle righe è stato scelto `getline`, perché gestisce righe di lunghezza variabile senza imporre un buffer fisso. L'alternativa era usare `fgets`, ma avrebbe richiesto logica aggiuntiva per righe più lunghe del buffer.
- La funzione di lettura distingue tra `path`, usato dal framework per aprire il file, e `file_name`, usato come nome logico serializzato nel record e visibile al mapper.
- Gli errori vengono propagati tramite valore di ritorno `-1` ed `errno`, senza stampare direttamente dalla libreria. La diagnostica esplicita andrà eventualmente collegata al sistema di log previsto dagli attributi.
- `mr_line_item_t` usa puntatori `const char *` perché le funzioni di scrittura dei record leggono i byte ma non modificano i buffer.

### Verifiche

- I test sono stati compilati ed eseguiti nel dev container dal programmatore.
- Non sono stati eseguiti comandi `make` sulla macchina host.
- È stato controllato che il working tree fosse pulito prima dell'aggiornamento del diario.

### Note aperte

- È emerso un problema di ownership su `mr_line_item_t`: la stessa struct rappresenta sia viste non proprietarie, usate durante la scrittura, sia dati allocati da `read_line_record`, che devono essere liberati.
- Prima di costruire il processo mapper e la coda delle righe, va scelta una politica esplicita per l'ownership, per esempio:
  - mantenere un solo tipo ma introdurre funzioni helper chiare per distruzione/copia;
  - separare i tipi tra vista da serializzare e record deserializzato proprietario.
- La scelta va fatta presto per evitare ambiguità su chi deve liberare `file_name` e `line` nei thread mapper.

### Prossimi passi

1. Implementare il riconoscimento di `input_path` come file regolare o directory.
2. Per il caso file regolare, chiamare la funzione di lettura già implementata usando come nome logico il basename del file.
3. Per il caso directory, raccogliere solo i file regolari diretti e processarli in ordine lessicografico.
4. Prima di integrare le code del mapper, chiarire definitivamente l'ownership dei record di riga.

### Punti da saper spiegare

- Perché `getline` richiede il controllo di `ferror` dopo il ciclo.
- Perché il newline non fa parte della riga logica.
- Differenza tra path fisico e filename logico.
- Perché una libreria propaga errori con `errno` invece di stampare.
- Quale rischio nasce quando una struct con puntatori è usata sia come vista non proprietaria sia come contenitore proprietario.

## 2026-06-03

### Stato attuale

- L'interfaccia pubblica minima e presente in `include/mr.h`.
- La gestione degli attributi e del ciclo di vita base (`mr_create`, `mr_destroy`) e implementata in `src/mr.c`.
- `mr_start` valida i parametri, ma non avvia ancora la pipeline MapReduce.
- Sono presenti funzioni interne `readn` e `writen` per gestire letture e scritture complete su file descriptor.
- È stato introdotto il formato interno per serializzare le righe dal processo principale al mapper: header con lunghezze e numero di riga, seguito dai byte di nome file e riga.
- Sono presenti le funzioni interne `write_line_record` e `read_line_record` per scrivere e leggere record di riga su file descriptor.
- È presente una prima coda circolare protetta con `mtx_t` e `cnd_t` per il pattern produttore-consumatore nel mapper.
- I test coprono attributi, ciclo di vita, comportamento provvisorio di `mr_start`, funzioni di I/O interne e serializzazione/deserializzazione delle righe.

### Requisiti da tenere fermi

- Usare processi con `fork()`, pipe, `dup2()` e `waitpid()`.
- Usare thread C11 da `<threads.h>`, non pthread.
- Non usare `exec()`, socket o memoria condivisa.
- Trattare `processed_token` e risultati come byte opachi con lunghezza nota.
- Trasmettere sulle pipe dati serializzati, mai struct con puntatori interni.
- Segnalare la fine dei flussi chiudendo le pipe, non con messaggi speciali.
- Garantire output deterministico, ad esempio ordinando per token.

### Prossimi passi ragionevoli

1. Definire il formato binario interno per coppie intermedie e risultati.
2. Implementare la lettura dell'input: file singolo, poi directory non ricorsiva in ordine lessicografico.
3. Costruire la pipeline di processi dentro `mr_start`.
4. Implementare il processo mapper con thread lettore, coda righe e worker mapper.
5. Implementare il processo reducer con raggruppamento per token e worker reducer.
6. Scrivere il formato di output finale e documentarlo.
7. Aggiungere il log di esecuzione.
8. Aggiungere almeno un esempio applicativo, preferibilmente il conteggio dei token.

### Punti da saper spiegare

- Perche le struct con puntatori non possono essere scritte direttamente su pipe.
- Come la chiusura corretta dei descrittori evita blocchi in attesa di EOF.
- Quale thread produce e quale thread consuma in ogni processo.
- Come vengono gestiti byte nulli dentro valori intermedi e risultati.
- Dove viene garantito il determinismo dell'output.

### Note aperte

- La coda corrente e specifica per le righe del mapper; il reducer probabilmente richiedera una struttura separata per gruppi o risultati.
- `mr_attr_set_log_file` salva il puntatore ricevuto senza copiarne il contenuto: va deciso se questo contratto e sufficiente o se conviene duplicare la stringa.
- Alcuni commenti presenti in `src/mr.c` sono utili come appunti di sviluppo, ma andranno rivisti prima della consegna per mantenerli essenziali.
