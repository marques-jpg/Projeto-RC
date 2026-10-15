# Report
Date: Thu 15 Oct 2026 05:21:45 AM WEST  
Repo: git@gitlab.rnl.tecnico.ulisboa.pt:rc/rc-26-27/ist1113868-proj1.git  
Commit: 9ff9fa87a5277f9262a309777e51b5805383c8a5  

## Build
* Found `Makefile`.
* Build succeeded.
* Found `file-sender`.
* Found `file-receiver`.

## Tests
| Test | Result |
| ---- |:------:|
| Sending small text file | OK |
| Sending binary file | OK |
| Sending 500 byte file | OK |
| Sending 1000 byte file | OK |
| Stop & Wait. No Loss | **FAIL** |
| Stop & Wait. Loss | **FAIL** |
| Go Back N. No Loss | **FAIL** |
| Go Back N. Loss | **FAIL** |
| Selective Repeat. No Loss | **FAIL** |
| Selective Repeat. Loss | **FAIL** |
| Message format | **FAIL** |

## Submission
* **`project1-submission` tag missing. Project not yet submitted.**
