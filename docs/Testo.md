# Progetto di Laboratorio 2 A
## Framework per analisi di file di testo adottando il paradigma MapReduce
**a.a. 2025-26**

---

## Premessa

Il progetto descritto in questo documento è valevole per il superamento della prova di Laboratorio 2 A. Il testo deve essere letto e compreso nella sua interezza. Il progetto consiste nella realizzazione di un piccolo framework, scritto in linguaggio C, per l'analisi di file testuali su un singolo calcolatore. Il framework deve poter essere utilizzato, come una qualunque libreria, da un normale programma C. Il programma che usa il framework dovrà fornire due funzioni applicative: una funzione **mapper**, che elabora una riga di un file e produce informazioni associate ai token contenuti in quella riga, e una funzione **reducer**, che combina tutte le informazioni associate allo stesso token e produce il risultato finale relativo a quel token. Il framework dovrà occuparsi dell'esecuzione concorrente, della comunicazione tramite pipe, della creazione dei processi e della sincronizzazione dei thread; le funzioni applicative dovranno invece descrivere l'analisi specifica da svolgere sui dati.

Non sono ammessi socket, memoria condivisa, `exec()` o thread POSIX pthread. Il progetto deve usare processi creati con `fork()`, pipe, `dup2()`, `waitpid()` e thread C11. È consentito l'uso dei semafori POSIX.

---

## 1. Introduzione concettuale al modello MapReduce

Il progetto si ispira a un modello di elaborazione chiamato **MapReduce**. Non è necessario conoscere preventivamente i dettagli di tale modello: in questa sezione ne viene data una descrizione concettuale, limitata agli aspetti necessari per comprendere l'assegnamento. MapReduce nasce per una classe molto comune di problemi: elaborare una grande quantità di dati, spesso testuali, suddividendo il lavoro in parti indipendenti, per poi combinare i risultati parziali in un risultato finale. L'idea non è legata a uno specifico linguaggio di programmazione o a una particolare tecnologia: è un modo di organizzare una computazione. Il principio fondamentale è separare il problema in due fasi:

1. una **fase di map**, nella quale i dati di input vengono trasformati in risultati intermedi;
2. una **fase di reduce**, nella quale i risultati intermedi appartenenti alla stessa categoria vengono combinati.

Fra queste due fasi esiste una terza operazione, spesso implicita ma essenziale: il **raggruppamento** dei risultati intermedi. Questa operazione raccoglie insieme tutti i dati che dovranno essere combinati dallo stesso reducer. Un modo utile per interpretare MapReduce è pensare ai risultati intermedi come coppie:

```
⟨chiave, valore⟩
```

La chiave serve a stabilire quali valori devono essere combinati insieme. Il valore contiene invece l'informazione prodotta durante la fase di map. La funzione di map decide come produrre queste coppie; il sistema di esecuzione decide come raggrupparle; la funzione di reduce decide come combinare i valori di uno stesso gruppo. La fase di map può essere eseguita in parallelo perché ogni porzione di input può essere trasformata indipendentemente dalle altre. Due mapper diversi possono quindi lavorare su dati diversi senza doversi coordinare.

La fase di raggruppamento non consiste semplicemente nel mettere in ordine una lista di coppie. In un sistema con più mapper e più reducer, ogni mapper può produrre coppie associate a chiavi diverse; tali coppie devono essere inoltrate al reducer responsabile della rispettiva chiave. Il requisito fondamentale è che tutte le coppie con la stessa chiave siano elaborate dallo stesso reducer, anche se sono state prodotte da mapper diversi. La scelta del reducer a cui inviare una coppia viene tipicamente effettuata tramite una funzione di partizionamento, per esempio basata su una funzione hash applicata alla chiave.

### 1.1 Responsabilità

Dal punto di vista concettuale, MapReduce distingue tre responsabilità:

- la **funzione map** decide come trasformare l'input in risultati intermedi;
- il **sistema di esecuzione** raccoglie e raggruppa i risultati intermedi in base alla chiave;
- la **funzione reduce** decide come combinare i valori associati alla stessa chiave.

Nel contesto di questo progetto non verrà realizzato un sistema distribuito: tutta l'esecuzione avviene sulla stessa macchina. Inoltre, per limitare la complessità, il progetto richiede un solo processo mapper e un solo processo reducer, entrambi multithread.

---

## 2. Descrizione generale del progetto

Si realizzi un framework denominato **libmr**, utilizzabile da programmi C esterni tramite un header pubblico `mr.h`. Il framework deve permettere di analizzare uno o più file di testo secondo il seguente modello:

```
file_line → ⟨token, processed_token⟩* → ⟨token, processed_token[]⟩ → result
```

Il programma che usa il framework fornisce due funzioni:

- una **funzione mapper**, che riceve una riga logica di un file e può emettere zero o più coppie `⟨token, processed_token⟩`;
- una **funzione reducer**, che riceve un token e tutti i valori associati a quel token, e produce uno o più risultati finali.

Il framework deve occuparsi di:

- leggere l'input di testo una riga alla volta;
- creare la pipeline di processi;
- trasferire ogni riga al processo mapper tramite pipe;
- trasferire le coppie prodotte dal mapper al processo reducer tramite pipe;
- raggruppare i valori per token nel processo reducer;
- invocare la funzione reducer sui gruppi completi;
- raccogliere i risultati finali e scriverli in output;
- gestire correttamente errori, chiusura e duplicazione dei descrittori, sincronizzazione e terminazione.

Il framework **non deve contenere logica specifica** per il conteggio delle parole o per altri esempi forniti dai docenti. In fase di valutazione potranno essere usate funzioni mapper e reducer diverse da quelle di esempio, purché conformi all'interfaccia pubblica.

---

## 3. Token, valori intermedi e risultati

Il framework opera su file testuali. Ogni riga letta dai file viene inviata al mapper. Il mapper può emettere coppie della forma:

```
⟨token, processed_token⟩
```

Il **token** è una sequenza non vuota di caratteri alfanumerici ASCII: `A-Z`, `a-z`, `0-9`. Il token viene usato dal framework come chiave di raggruppamento. Tutti i valori associati allo stesso token devono essere raccolti insieme e consegnati alla funzione reducer in un'unica invocazione. Nel contratto dell'interfaccia pubblica, il token deve essere rappresentato come una stringa C valida, terminata da `'\0'`, composta esclusivamente da caratteri alfanumerici ASCII. Il carattere terminatore non fa parte del token logico.

Il valore **processed_token** è invece un dato opaco. Il framework non deve interpretarlo. In particolare, non deve assumere che sia un intero, una stringa C, un dato stampabile, un dato di dimensione fissa, o una sequenza terminata da `'\0'`. Il framework deve trattarlo sempre come una sequenza di byte di lunghezza nota. Lo stesso vale per i risultati prodotti dal reducer.

---

## 4. Input

L'input dell'elaborazione è costituito da uno o più file di testo. Il framework deve poter ricevere come input:

- un singolo file regolare;
- una directory contenente file regolari.

Nel caso di una directory, il framework deve elaborare tutti i file regolari contenuti direttamente in essa (non è richiesta la scansione ricorsiva). I file devono essere considerati in **ordine lessicografico** rispetto al loro nome, per garantire determinismo.

Il framework legge ciascun file come una sequenza di **righe logiche**. Una riga logica è normalmente separata dalla successiva dal carattere `'\n'`, che non fa parte del contenuto passato al mapper. L'ultima riga di un file può anche non essere terminata da `'\n'`. Devono essere gestiti correttamente almeno i seguenti casi:

- file vuoti;
- righe vuote;
- file contenenti una sola riga;
- ultima riga di un file non terminata da `'\n'`.

La struttura dati passata dal framework alla funzione mapper per rappresentare una riga logica è la seguente:

```c
/* Riga logica di un file. */
typedef struct {
    const char *file_name;
    size_t       file_name_len;
    unsigned long line_number;
    const char *line;
    size_t       line_len;
} mr_file_line_t;
```

La struttura `mr_file_line_t` non deve essere usata come formato binario da scrivere direttamente sulla pipe, poiché i campi `file_name` e `line` sono puntatori validi solo nello spazio di indirizzamento del processo che li ha creati. Fra processo principale e processo mapper la riga deve quindi essere trasmessa in forma serializzata, usando lunghezze e byte effettivi. Il carattere di fine riga non fa parte del contenuto logico della riga passato al mapper.

---

## 5. Architettura dei processi

Il framework deve realizzare una **pipeline locale** composta da tre processi. Quando viene avviata un'elaborazione, il framework deve creare due processi figli:

- un **processo mapper**, che riceve righe logiche dal processo principale, esegue la fase di mapping usando più thread C11 e produce coppie `⟨token, processed_token⟩`;
- un **processo reducer**, che riceve le coppie prodotte dal mapper, raggruppa i valori per token, esegue la fase di reducing usando più thread C11 e produce i risultati finali.

La comunicazione fra i tre processi deve avvenire tramite pipe. Sono necessarie almeno tre pipe:

- una pipe dal processo principale al processo mapper;
- una pipe dal processo mapper al processo reducer;
- una pipe dal processo reducer al processo principale.

```
Processo principale → [pipe: righe serializzate] → Processo mapper (multithread C11)
                                                         ↓ [pipe: coppie serializzate]
                                                    Processo reducer (multithread C11)
                                                         ↓ [pipe: risultati serializzati]
                                                    Processo principale (raccolta output)
```

Un possibile schema di inizializzazione delle pipe:

```c
int main_to_mapper[2];
int mapper_to_reducer[2];
int reducer_to_main[2];

pipe(main_to_mapper);
pipe(mapper_to_reducer);
pipe(reducer_to_main);
```

Dopo la creazione delle pipe, il framework deve creare il processo mapper tramite `fork()`. Nel processo figlio, lo standard input deve essere collegato alla pipe proveniente dal processo principale, mentre lo standard output deve essere collegato alla pipe diretta al reducer:

```c
pid_t mapper_pid = fork();

if (mapper_pid == 0) {
    dup2(main_to_mapper[0], STDIN_FILENO);
    dup2(mapper_to_reducer[1], STDOUT_FILENO);

    /*
     * Chiudere qui tutti i descrittori non necessari.
     */

    mapper_process_main(...);
    _exit(0);
}
```

Analogamente per il processo reducer:

```c
pid_t reducer_pid = fork();

if (reducer_pid == 0) {
    dup2(mapper_to_reducer[0], STDIN_FILENO);
    dup2(reducer_to_main[1], STDOUT_FILENO);

    /*
     * Chiudere qui tutti i descrittori non necessari.
     */

    reducer_process_main(...);
    _exit(0);
}
```

Nel processo principale devono rimanere aperti solo:

- il lato di scrittura della pipe verso il mapper;
- il lato di lettura della pipe proveniente dal reducer.

Tutti gli altri descrittori devono essere chiusi. La corretta chiusura dei descrittori è parte essenziale del progetto: un descrittore lasciato aperto nel processo sbagliato può impedire la ricezione dell'EOF e causare un blocco permanente della pipeline.

Il framework **non deve usare `exec()`**. Con `fork()`, i processi figli ereditano il codice, la configurazione e i puntatori a funzione necessari per invocare le callback. Il framework non deve eseguire `fork()` da un processo che abbia già creato thread C11 attivi.

Riassumendo, durante l'avvio dell'elaborazione il framework deve:

1. creare le pipe necessarie;
2. creare il processo mapper tramite `fork()`;
3. creare il processo reducer tramite `fork()`;
4. usare `dup2()` nei figli per collegare le pipe a stdin e stdout;
5. chiudere in ogni processo tutti i descrittori non utilizzati;
6. avviare nei processi figli i rispettivi thread C11;
7. inviare dal processo principale le righe serializzate al mapper;
8. raccogliere nel processo principale i risultati serializzati prodotti dal reducer;
9. attendere la terminazione dei processi figli tramite `waitpid()`.

### 5.1 Segnalazione della fine dell'input

La fine dell'input non deve essere segnalata tramite messaggi speciali, ma tramite la **chiusura delle pipe** e la conseguente ricezione di EOF. Il flusso previsto è il seguente:

1. il processo principale legge tutti i file di input e invia al processo mapper le righe serializzate;
2. quando non ci sono più righe da inviare, il processo principale chiude il lato di scrittura della pipe verso il mapper;
3. il processo mapper riceve EOF sul proprio standard input;
4. il thread lettore del processo mapper marca la coda interna delle righe come chiusa;
5. i thread mapper completano l'elaborazione delle righe già presenti nella coda e terminano;
6. solo dopo la terminazione di tutti i thread mapper, il processo mapper chiude il proprio standard output (la pipe verso il reducer);
7. il processo reducer riceve EOF sul proprio standard input;
8. a questo punto il reducer sa che non arriveranno più coppie `⟨token, processed_token⟩`;
9. il processo reducer completa il raggruppamento, invoca la funzione reducer sui gruppi completi e scrive i risultati finali;
10. infine il reducer chiude il proprio standard output, permettendo al processo principale di ricevere EOF sulla pipe dei risultati.

---

## 6. Thread C11

Il processo mapper e il processo reducer devono essere multithread e devono usare i thread C11 tramite l'header:

```c
#include <threads.h>
```

Non devono essere usati `pthread_create`, `pthread_mutex_t` o `pthread_cond_t`. Le funzioni passate a `thrd_create()` devono avere firma compatibile con `thrd_start_t`:

```c
static int mapper_worker_main(void *arg);
static int reducer_worker_main(void *arg);
static int reader_main(void *arg);
```

Le code interne ai processi mapper e reducer sono strutture dati normali, accessibili dai thread dello stesso processo e protette tramite `mtx_t` e `cnd_t`. Il parametro `queue_size` indica la capacità massima delle code interne. Se una coda è piena, il thread produttore deve attendere; se una coda è vuota, il thread consumatore deve attendere tramite condition variable C11.

### 6.1 Processo mapper

Una possibile organizzazione del processo mapper:

- un **thread lettore** legge le righe da stdin e le inserisce in una coda condivisa;
- un certo numero di **thread mapper** estrae righe dalla coda, invoca la funzione mapper e produce coppie `⟨token, processed_token⟩`;
- le coppie prodotte vengono scritte sullo standard output del processo mapper; quando tutti i thread mapper sono terminati, il processo mapper chiude tale pipe.

Poiché più thread mapper possono produrre coppie contemporaneamente, la scrittura verso la pipe deve essere sincronizzata.

### 6.2 Processo reducer

Una possibile organizzazione del processo reducer:

- un **thread lettore** legge le coppie da stdin;
- il reducer raggruppa tutti i valori associati allo stesso token;
- quando l'input è terminato, vengono creati gruppi della forma `⟨token, processed_token[]⟩`;
- i **thread reducer** elaborano i gruppi invocando la funzione reducer fornita dal programma utente;
- i risultati prodotti vengono raccolti e scritti verso lo standard output del processo reducer.

La funzione reducer deve essere invocata **solo dopo** che il framework ha raccolto tutti i valori associati a un token.

---

## 7. Protocollo interno sulle pipe

La comunicazione tra processi deve usare un **protocollo esplicito basato su lunghezze**. Per esempio, una coppia prodotta dal mapper può essere rappresentata da un header:

```c
typedef struct {
    int token_len;
    int value_len;
} mr_pair_header_t;
```

seguito da:

- `token_len` byte contenenti il token;
- `value_len` byte contenenti il valore opaco.

La lunghezza `token_len` non include il terminatore `'\0'`. Il processo che riceve il token deve quindi allocare un buffer di almeno `token_len + 1` byte e aggiungere localmente il terminatore nullo. Il valore `value_len` indica esattamente il numero di byte del valore opaco e non implica alcun terminatore.

Il protocollo deve gestire correttamente **letture e scritture parziali**. Devono essere realizzate funzioni ausiliarie equivalenti a:

```c
ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);
```

---

## 8. Output

Il file di output deve essere prodotto dal framework. Ogni record di output deve contenere almeno:

- la lunghezza del token;
- il token;
- la lunghezza del risultato;
- i byte del risultato.

Il framework deve garantire un **output deterministico**. A parità di input, funzioni mapper/reducer e parametri del framework, il file di output deve essere identico tra esecuzioni diverse. Una scelta semplice è scrivere i risultati ordinati lessicograficamente per token. Se il reducer emette più risultati per lo stesso token, l'ordine relativo deve essere definito e documentato. La lunghezza del token nel formato di output non include il terminatore `'\0'`.

---

## 9. Analisi di esempio

Deve essere fornito almeno un programma di esempio che usi il framework. L'esempio consigliato è il **conteggio delle occorrenze dei token**.

In questo esempio, il mapper emette per ogni token il valore intero 1, serializzato come byte:

```c
int one = 1;
emit(token, &one, sizeof(one), emit_arg);
```

Il reducer riceve tutti i valori associati allo stesso token, li interpreta come interi e produce il totale. Il framework non deve contenere logica specifica per il conteggio delle parole.

---

## 10. Precisazioni importanti sul contratto del framework

- il framework deve trattare ogni `processed_token` come una sequenza opaca di byte di lunghezza nota;
- non è garantito che i valori intermedi siano stringhe C terminate da `'\0'`;
- i valori intermedi possono contenere byte nulli;
- il framework non deve usare funzioni come `strlen`, `strcpy`, `strcmp` o `printf("%s")` sui valori intermedi;
- il token è l'unico campo interpretato dal framework, ed è usato esclusivamente per il raggruppamento;
- la funzione reducer deve essere invocata una sola volta per ogni token distinto, ricevendo tutti i valori associati a quel token;
- la comunicazione su pipe deve gestire correttamente letture e scritture parziali;
- l'uso di thread e processi non autorizza race condition, perdita di risultati, duplicazioni o dipendenza dall'ordine di scheduling;
- nei processi mapper e reducer lo standard output è riservato al protocollo interno del framework; eventuali messaggi diagnostici devono usare il log del framework o stderr;
- le lunghezze contenute negli header del protocollo sono espresse come valori di tipo `int`; ogni lunghezza ricevuta deve essere controllata prima di essere usata (non sono ammessi valori negativi); prima di convertire tali valori a `size_t`, il framework deve verificarne la validità.

---

## 11. Log di esecuzione

Il framework deve produrre un file di log. Il nome del file di log può essere configurato tramite `mr_attr_set_log_file()`. Se non specificato, il framework deve usare un nome di default ragionevole, ad esempio `mr.log`.

Ogni riga del log deve avere un formato chiaro e documentato, ad esempio:

```
[timestamp] [processo] [thread] [evento] messaggio
```

Devono essere registrati almeno:

- creazione delle pipe;
- creazione dei processi mapper e reducer;
- avvio e terminazione dei thread;
- apertura e chiusura dei file di input e output;
- numero di righe inviate al mapper;
- numero di coppie prodotte dal mapper;
- numero di token distinti raggruppati dal reducer;
- numero di risultati finali prodotti;
- errori rilevati.

Se più thread o più processi scrivono sul log, l'accesso deve essere sincronizzato. In caso di scritture da processi diversi, devono essere usati meccanismi adatti alla sincronizzazione interprocesso (es. semafori POSIX, lock su file, o raccolta da un unico processo).

---

## 12. Requisiti tecnici

- Linguaggio: C
- Piattaforma: Linux Ubuntu 24.04
- Ogni chiamata di sistema deve essere opportunamente controllata
- Si consiglia l'uso di macro o funzioni wrapper per uniformare la gestione degli errori

---

## 13. Test e valutazione

Il progetto deve produrre una libreria statica `libmr.a`. La valutazione potrà compilare programmi di test esterni che includono `mr.h`, collegano `libmr.a` e forniscono mapper e reducer propri. Il framework sarà valutato anche con mapper e reducer diversi da quelli distribuiti come esempio. Il target `make test` deve eseguire una batteria di test automatizzati predisposti dallo studente.

---

## 14. Addendum (per chi non ha superato un numero congruo di prove in itinere)

Agli studenti che non abbiano superato un numero congruo di prove in itinere sarà richiesto di implementare le seguenti funzionalità aggiuntive:

- **scansione ricorsiva** delle directory di input;
- possibilità di eseguire **più elaborazioni indipendenti** nello stesso processo chiamante, senza interferenze fra le diverse istanze di `mr_t`;
- **statistiche finali** in un file separato, contenente almeno tempi di esecuzione, numero di righe lette, numero di coppie prodotte, numero di token distinti e numero di risultati emessi;
- possibilità per l'utente di specificare una **funzione di hashing deterministica** per l'assegnazione dei token ai thread reducer interni al processo reducer.

Per l'addendum, la definizione di `mr_attr_t` nell'interfaccia pubblica deve essere estesa come segue:

```c
typedef size_t (*mr_hash_t)(
    const char *token,
    size_t       token_len,
    void        *user_arg
);

typedef struct {
    size_t mapper_threads;
    size_t reducer_threads;
    size_t queue_size;
    const char *log_file;

    mr_hash_t hash;
    void     *hash_arg;
} mr_attr_t;

int mr_attr_set_hash_function(
    mr_attr_t *attr,
    mr_hash_t  hash,
    void      *hash_arg
);
```

---

## 15. Consegna

Il progetto deve essere consegnato come archivio `.zip`. L'archivio deve contenere almeno:

- file sorgenti `.c` e `.h`;
- directory `include/` contenente `mr.h`;
- directory `src/` contenente l'implementazione del framework;
- directory `examples/` contenente almeno un esempio d'uso;
- `Makefile` e `README`;
- relazione in formato PDF.

Il `Makefile` deve prevedere almeno i seguenti target:

- `make`: compila il framework, la libreria `libmr.a` e almeno un esempio;
- `make test`: esegue i test automatici;
- `make clean`: rimuove file oggetto, eseguibili e file temporanei.

---

## 16. Relazione

La relazione deve essere in formato PDF, di massimo 10 pagine, e deve contenere:

- descrizione dell'architettura generale del framework;
- descrizione dell'interfaccia pubblica;
- organizzazione dei processi e uso di `fork()`, `pipe()`, `dup2()` e `waitpid()`;
- organizzazione dei thread C11 nel mapper e nel reducer;
- struttura delle code interne e meccanismi di sincronizzazione;
- formato dei messaggi scambiati sulle pipe;
- struttura dati usata per il raggruppamento per token;
- formato del file di output;
- formato del file di log;
- descrizione dei test realizzati.

---

## 17. Note finali

Il progetto deve essere svolto in autonomia. Il codice consegnato deve essere comprensibile, commentato in modo adeguato e organizzato in più file secondo criteri ragionevoli. L'uso di strumenti automatici di supporto alla programmazione non solleva lo studente dalla responsabilità di comprendere, verificare e saper spiegare il codice consegnato. La valutazione potrà includere test con mapper e reducer non noti in anticipo e domande sulle scelte progettuali adottate.

---

## Appendice A — Interfaccia pubblica richiesta

Il framework deve esporre un header pubblico `include/mr.h`. Di seguito è riportata l'interfaccia minima richiesta:

```c
#ifndef MR_H
#define MR_H

#include <stddef.h>

/* Handle opaco di una elaborazione. */
typedef struct mr *mr_t;

/*
 * Attributi di configurazione del framework.
 *
 * mapper_threads : numero di thread usati nel processo mapper.
 * reducer_threads: numero di thread usati nel processo reducer.
 * queue_size     : dimensione delle code interne usate dal framework.
 * log_file       : nome del file di log, oppure NULL per usare il nome di default.
 */
typedef struct {
    size_t      mapper_threads;
    size_t      reducer_threads;
    size_t      queue_size;
    const char *log_file;
} mr_attr_t;

/*
 * Riga logica di un file, vista dalla funzione mapper.
 */
typedef struct {
    const char   *file_name;
    size_t        file_name_len;
    unsigned long line_number;
    const char   *line;
    size_t        line_len;
} mr_file_line_t;

/*
 * Valore opaco associato a un token.
 * Il framework non interpreta il contenuto di data.
 * Se size vale 0, data può essere NULL.
 */
typedef struct {
    const void *data;
    size_t      size;
} mr_value_t;

/*
 * Funzione usata dal mapper per emettere una coppia <token, valore>.
 * token deve essere una stringa C valida, terminata da '\0',
 * composta soltanto da caratteri alfanumerici ASCII.
 */
typedef int (*mr_emit_pair_t)(
    const char *token,
    const void *value,
    size_t      value_size,
    void       *emit_arg
);

/*
 * Funzione usata dal reducer per emettere un risultato finale.
 */
typedef int (*mr_emit_result_t)(
    const char *token,
    const void *result,
    size_t      result_size,
    void       *emit_arg
);

/*
 * Funzione mapper fornita dal programma utente.
 */
typedef int (*mr_mapper_t)(
    const mr_file_line_t *line,
    mr_emit_pair_t        emit,
    void                 *emit_arg,
    void                 *user_arg
);

/*
 * Funzione reducer fornita dal programma utente.
 */
typedef int (*mr_reducer_t)(
    const char        *token,
    const mr_value_t  *values,
    size_t             values_count,
    mr_emit_result_t   emit,
    void              *emit_arg,
    void              *user_arg
);

int mr_attr_init(mr_attr_t *attr);
int mr_attr_destroy(mr_attr_t *attr);

int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_queue_size(mr_attr_t *attr, size_t n);
int mr_attr_set_log_file(mr_attr_t *attr, const char *path);

int mr_create(
    mr_t         *mr,
    const mr_attr_t *attr,
    mr_mapper_t   mapper,
    mr_reducer_t  reducer,
    void         *user_arg
);

int mr_start(mr_t mr, const char *input_path, const char *output_path);
int mr_destroy(mr_t mr);

#endif
```

Le funzioni del framework devono restituire `0` in caso di successo e `-1` in caso di errore, impostando `errno` ove appropriato. Il tipo `mr_t` è opaco; il tipo `mr_attr_t` è completamente definito nell'header. La funzione `mr_attr_init()` deve inizializzare gli attributi con valori di default validi (numero di thread mapper e reducer almeno 1). Le funzioni `mr_attr_set_*()` devono rifiutare valori non validi. La funzione `mr_start()` è bloccante e restituisce il controllo al chiamante solo quando l'elaborazione è terminata oppure quando si verifica un errore.

### A.1 Validità dei puntatori e copia dei dati

I puntatori passati dal framework alle funzioni mapper e reducer sono validi soltanto durante l'invocazione della callback. Il framework deve copiare i dati ricevuti tramite `emit` prima che la funzione di emissione ritorni. Il campo `user_arg` viene passato senza interpretazione; poiché il framework usa `fork()`, eventuali modifiche effettuate a oggetti puntati da `user_arg` nei processi figli non sono visibili nel processo principale.

---

## Appendice B — Uso previsto del framework

Un programma applicativo che usa il framework dovrà avere una struttura simile alla seguente:

```c
#include <stdio.h>
#include "mr.h"

int main(int argc, char **argv) {
    mr_t     mr;
    mr_attr_t attr;

    if (mr_attr_init(&attr) == -1) {
        perror("mr_attr_init");
        return 1;
    }

    if (mr_attr_set_mapper_threads(&attr, 4) == -1) {
        perror("mr_attr_set_mapper_threads");
        mr_attr_destroy(&attr);
        return 1;
    }

    if (mr_attr_set_reducer_threads(&attr, 4) == -1) {
        perror("mr_attr_set_reducer_threads");
        mr_attr_destroy(&attr);
        return 1;
    }

    if (mr_attr_set_queue_size(&attr, 64) == -1) {
        perror("mr_attr_set_queue_size");
        mr_attr_destroy(&attr);
        return 1;
    }

    if (mr_attr_set_log_file(&attr, "mr.log") == -1) {
        perror("mr_attr_set_log_file");
        mr_attr_destroy(&attr);
        return 1;
    }

    if (mr_create(&mr, &attr, my_mapper, my_reducer, NULL) == -1) {
        perror("mr_create");
        mr_attr_destroy(&attr);
        return 1;
    }

    if (mr_start(mr, "input", "output.mro") == -1) {
        perror("mr_start");
        mr_destroy(mr);
        mr_attr_destroy(&attr);
        return 1;
    }

    mr_destroy(mr);
    mr_attr_destroy(&attr);

    return 0;
}
```

### B.1 Estensione dell'interfaccia per l'addendum

Per gli studenti tenuti a realizzare l'addendum, l'interfaccia pubblica dovrà essere estesa per consentire di specificare una funzione di hashing deterministica:

```c
/*
 * Funzione di hashing opzionale per partizionare i token
 * fra i thread reducer interni al processo reducer.
 * La funzione deve essere deterministica.
 */
typedef size_t (*mr_hash_t)(
    const char *token,
    size_t      token_len,
    void       *user_arg
);

int mr_attr_set_hash_function(
    mr_attr_t *attr,
    mr_hash_t  hash,
    void      *hash_arg
);
```

Se non viene specificata alcuna funzione di hashing, il framework deve usare una funzione di default. La funzione di hashing non deve modificare il token e non deve dipendere dall'ordine di esecuzione dei thread.
