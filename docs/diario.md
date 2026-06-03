# Diario tecnico

Questo file raccoglie lo stato operativo del progetto e andra aggiornato durante lo sviluppo. Non sostituisce il testo ufficiale: la fonte di verita resta `docs/Testo.md`, limitatamente al progetto base.

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
