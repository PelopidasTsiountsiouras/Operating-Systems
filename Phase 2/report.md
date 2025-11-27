# TinyShell - Phase 2 Report

## Ανακατεύθυνση και Αγωγοί (Redirection and Pipelines)

### 1. **Εισαγωγή**

Στη δεύτερη φάση του TinyShell προστέθηκαν μηχανισμοί ανακατεύθυνσης εξόδου και υποστήριξη αγωγών (pipes), με στόχο τον εμπλουτισμό της λειτουργικότητας του απλού κελύφους που αναπτύχθηκε στην Phase 1.

Το TinyShell εξακολουθεί να παραμένει ελαφρύ και εκπαιδευτικό, ακολουθώντας αυστηρά τις POSIX κλήσεις συστήματος (fork, execve, pipe, dup2, wait).

### 2. **Νέες Λειτουργίες Phase 2**

#### 2.1 **Ανακατεύθυνση Εξόδου**
Υποστηρίζονται οι δύο βασικοί τελεστές εξόδου:
- `>` : δημιουργία/αντικατάσταση αρχείου
- `>>` : προσθήκη στο τέλος αρχείου (append mode)

Παράδειγμα:
```sh
ls -l > out.txt
ls -l >> out.txt
```

Το Tinyshell ανοίγει το αρχείο με `open()` και το αντικαθιστά στο `STDOUT_FILENO` με `dup2()` πριν το `execve()`.

#### 2.2 **Αγωγός δύο εντολών (single pipe)**

Υλοποιήθηκε υποστήριξη για μία εντολή pipe:
```sh
ls -l | grep ".txt"
```
Χρησιμοποιείται η POSIX `pipe()` για τη δημιουργία δύο file descriptors:
- one for read (pipes[0])
- one for write (pipes[1])

Ο πρώτος child process γράφει στο write-end, ο δεύτερος διαβάζει από το read-end.

#### 2.3 **Πολλαπλοί Αγωγοί (pipeline chain)**

Το shell υποστηρίζει N εντολές ενωμένες με αγωγούς:
```sh
cat file.txt | grep "pattern" | sort | uniq
```
Για κάθε δύο διαδοχικές εντολές δημιουργείται ένα pipe.
Για N στάδια απαιτούνται N−1 pipes.

Σε κάθε παιδί:
- Η είσοδος (stdin) προέρχεται από το προηγούμενο pipe
- Η έξοδος (stdout) κατευθύνεται προς το επόμενο pipe
- Η σύνδεση γίνεται με dup2() πριν το execve().

#### 2.4 **Συνδυασμός Pipes και Redirections**

Το σύστημα υποστηρίζει συνδυασμούς τύπου:
```sh
cat input.txt | grep hello >> results.txt
```
Η πολιτική του shell:
- Η **ανακατεύθυνση εφαρμόζεται μόνο στο τελευταίο στάδιο του pipeline**.
- Όλα τα προηγούμενα στάδια γράφουν σε pipes.
- Το τελευταίο στάδιο γράφει είτε σε pipe (αν υπάρχουν επόμενα στάδια) είτε στο redirected αρχείο.

### 3. **Parsing και Δομή Δεδομένων**

#### 3.1 **Tokenization**
Η είσοδος χωρίζεται σε tokens με βάση whitespace:
```c
split_line(line, argv);
```
#### 3.2 **Pipeline Parsing**
Το `parse_pipeline()` δημιουργεί μια δομή:
```c
typedef struct {
    Command cmds[MAX_CMDS];
    int n_cmds;
    char *outfile;
    int append;
} Pipeline;
```
Κάθε `Command` περιέχει μια λίστα argv tokens.

Το parsing:

- Διαχωρίζει τα stages με βάση το `|`
- Ανιχνεύει `>` & `>>`
- Ελέγχει συντακτικά λάθη (πολλαπλά pipes, πολλαπλά redirects κ.λπ.)

Παράδειγμα:
```bash
ls -l | grep .txt >> out.txt
```

→ 3 stages:

- Stage 0: ["ls", "-l"]
- Stage 1: ["grep", ".txt"]
- Stage 2: ["wc", "-l"]
- outfile="out.txt", append=1

### 4. **Εκτέλεση Pipelines**

#### 4.1 **Pipe Creations**

Για N εντολές:
```c
pipe(pipes[i]);
```
όπου `i = 0…N-2`.

#### 4.2 **Fork / Exec**

Για κάθε στάδιο:
```c
pid = fork();
```

Στο child:

- Αν δεν είναι το πρώτο στάδιο → `dup2(previous_pipe_read_end, STDIN)`.
- Αν δεν είναι το τελευταίο στάδιο → `dup2(next_pipe_write_end, STDOUT)`.
- Αν υπάρχει redirection → αντικαθιστά το STDOUT σε αρχείο.
- Κλείνει όλα τα pipes.
- Καλεί `execve()`.

Στο parent:

- Κλείνει όλα τα pipes μετά τη δημιουργία των παιδιών.
- Περιμένει όλα τα children με `waitpid()`.

### 5. **System Calls που χρησιμοποιούνται**

| Κλήση | Χρήση|
|-------|------|
| `fork()` | Δημιουργία child processes |
| `execve()` | Εκτέλεση νέου προγράμματος  |
| `pipe()` | Δημιουργία διαύλων επικοινωνίας |
| `dup2()` | Ανακατεύθυνση stdin/stdout |
| `open()` | Δημιουργία αρχείων για `>` και `>>`  |
| `close()` | Απελευθέρωση file descriptors |
| `wait()`/`waitpid()` | Συλλογή status παιδιών |
| `access()` | Αναζήτηση εκτελέσιμων στο `$PATH` |

### 6. **Παραδείγματα Εκτέλεσης**   

Ανακατεύθυνση εξόδου:
```powerhell
tinyshell$ ls -l > out.txt
```
Pipeline:
```powerhell
tinyshell$ ls | grep .c
```

Πολλοί pipeline stages:
```powerhell
tinyshell$ ls -l | grep .txt | wc -l
0
```

Συνδυασμός με redirection:
```powerhell
tinyshell$ cat data.txt | grep hello >> results.txt
```
### 7. **Έλεγχος Ορθής Λειτουργίας**

Τα tests που εκτελέστηκαν:

- `>` και `>>` → επιβεβαιώθηκαν.
- pipelines δύο εντολών → σωστά exit codes.
- pipelines πολλαπλών εντολών → σωστή μεταφορά ροής.
- συνδυασμός pipes + redirection → λειτουργεί όπως στο bash.
- διαχείριση λαθών (`command not found`, missing file) → γίνεται κανονικά.

Όλα τα αποτελέσματα συμφωνούν με τη συμπεριφορά πραγματικού Unix shell.

### 8. **Συμπεράσματα**
Η Phase 2 ολοκλήρωσε επιτυχώς τις βασικές δυνατότητες που απαιτούνται για ένα λειτουργικό Unix-like κέλυφος:

- υλοποίηση αγωγών (pipes)
- υλοποίηση ανακατευθύνσεων εξόδου
- υποστήριξη πολλαπλών σταδίων
- ορθή διαχείριση file descriptors

σωστός συνδυασμός pipelines + redirections

Το TinyShell πλέον προσεγγίζει τη συμπεριφορά των πραγματικών shells (bash, dash), παραμένοντας απλό και εξαιρετικά εκπαιδευτικό ως άσκηση διαχείρισης POSIX διεργασιών.