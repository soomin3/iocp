# DummyClient

`GameServer`의 부하·장애 검증을 위한 Windows C++20 콘솔 프로젝트입니다. `ConnectEx` / `WSARecv` / `WSASend`와 IOCP를 사용합니다. 접속마다 스레드를 만들지 않고 단일 이벤트 루프에서 연결과 타이머를 관리합니다. `Common/Protocol/`의 `Packet.h`, `PacketList.inc`, 결과 enum을 직접 사용합니다.

## 먼저 알아둘 제한

- 현재 로그인 기능은 `GameServer:8694`에 있습니다. `LoginServer`는 아직 클라이언트 로그인 엔드포인트가 아닙니다.
- 실제 서버 검증은 `DBServer` → `GameServer` → `DummyClient` 순서로 실행합니다. DB 계정 설정은 `DBServer`만 읽습니다.
- 현재 `GameServer/Headers/Network/IOCPManager.h`의 `USER_SIZE`는 **100**입니다. 5,000/10,000명 실제 동시 로그인 시험 전에는 서버 세션 풀 한도를 목표 이상으로 조정하고 서버를 다시 빌드해야 합니다. 더미 클라이언트를 추가하면서 서버 한도는 바꾸지 않았습니다.
- 로그인할 서로 다른 테스트 계정이 DB에 미리 있어야 합니다. 이 프로그램은 계정을 만들거나 DB를 변경하는 SQL을 실행하지 않습니다. 로그인 응답의 `needNickname=1`이면 실제 닉네임 생성 패킷을 보내므로 해당 계정의 닉네임은 저장됩니다.
- `lineage`는 리니지형 MMORPG의 장시간 다중 접속을 시험하기 위해 잡은 **5,000명 목표 프리셋**입니다. 리니지의 공식 수용 인원이나 실제 트래픽 재현 수치를 뜻하지 않습니다. 이동·전투 패킷이 없는 현재 서버에서는 주기적 월드 채팅을 사용합니다.
- 월드 채팅은 전체 사용자에게 방송됩니다. 모든 접속자가 1초마다 말하면 수신 부하는 대략 `접속 수²`에 비례합니다. 접속 수와 발신자 비율을 따로 지정할 수 있습니다.

## 빌드

Visual Studio C++ 데스크톱 개발 도구(v145), Windows SDK가 필요합니다. 외부 패키지는 없습니다. Developer PowerShell에서 저장소 루트를 기준으로 실행합니다.

```powershell
msbuild .\DummyClient\DummyClient.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
.\DummyClient\x64\Release\DummyClient.exe --help
```

솔루션 빌드에도 포함됩니다. 솔루션으로 빌드했다면 실행 파일은 `x64/Release/DummyClient.exe`입니다. 대규모 시험에는 Release x64를 사용하세요.

## 계정과 로그인

`dummy_1`, `dummy_2`, ...처럼 기존 테스트 계정 이름이 연속적이면 공통 비밀번호를 환경 변수로 전달합니다. 기본 닉네임은 `dc_1`, `dc_2`, ...입니다.

```powershell
$env:DUMMY_PASSWORD = '테스트 계정의 비밀번호'
.\DummyClient\x64\Release\DummyClient.exe --clients 20 --duration 60 --csv DummyClient/results/steady.csv
```

개별 계정은 `accounts.example.tsv`를 복사해 `accounts.local.tsv`로 준비합니다. UTF-8, 헤더 없는 `ID<TAB>비밀번호<TAB>닉네임` 형식이며 `#` 주석과 빈 줄을 허용합니다. BOM/CRLF도 허용합니다. `--clients` 이상의 행이 있어야 하며 앞에서부터 슬롯에 배정합니다. `mixed`의 비정상 패킷 슬롯도 행은 필요하지만 로그인하지 않습니다.

```powershell
.\DummyClient\x64\Release\DummyClient.exe --accounts DummyClient/accounts.local.tsv --clients 2 --duration 30
```

ID·비밀번호는 각각 UTF-8 최대 31바이트, 닉네임은 최대 63바이트/16문자입니다. 중복 ID·닉네임, 필드 길이, 잘못된 UTF-8은 실행 전에 거부합니다. 계정과 비밀번호는 로그/CSV에 출력하지 않습니다. 로컬 계정 파일과 결과 폴더는 Git에서 제외합니다.

흐름은 `TCP 접속 → CS_LOGIN_REQ → SC_LOGIN_RES → 필요 시 닉네임 요청/응답 → Ready → 주기적 채팅`입니다. 기존 닉네임은 그대로 사용합니다. 재접속할 때도 같은 계정으로 이 흐름을 다시 수행합니다. 인증 실패도 결과 코드별로 기록하고 설정된 지연 후 재시도합니다.

## 시나리오

| `--scenario` | 동작 |
|---|---|
| `steady` | 목표 슬롯 수만큼 접속·로그인 유지, 주기적 채팅. 예기치 않은 연결 종료도 재접속 |
| `connect-only` | TCP 연결만 유지. 계정 없이 네트워크 접속 수 시험 |
| `reconnect` | `--fault-every-ms`마다 Ready 중 `--disconnect-percent`를 `SO_LINGER={1,0}`으로 강제 종료(RST), 지연 후 재로그인 |
| `churn` | 초당 `--churn-rate`개의 Ready 연결을 FIN 종료하고 같은 슬롯을 지연 후 재접속. 대상은 순환 선택 |
| `malformed` | 별도 비인증 연결마다 아래 6종 패킷을 순환 전송하고 서버의 거부/종료를 확인 |
| `mixed` | 인덱스 0,20,40,... 슬롯은 비정상 패킷, 나머지는 정상 로그인·채팅 + 강제 종료 + Churn |

비정상 패킷은 헤더 크기 3, 알 수 없는 ID, 로그인 패킷 크기 불일치, 내부 `SD_LOGIN_REQ` 위조, NUL 종료 없는 로그인 문자열, 중간에서 끊긴 로그인 본문(12바이트 전송 후 FIN)입니다. NUL 종료 없는 문자열은 `INVALID_CREDENTIAL` 응답을 기대하고, 나머지는 연결 종료를 기대합니다. 타임아웃은 정상 거부로 계산하지 않습니다. 중간에서 끊긴 패킷의 FIN은 서버가 EOF를 처리하는지도 확인합니다. 서버가 본문을 무기한 기다리는 공격과는 다른 검사입니다.

```powershell
# 계정 없이 비정상 패킷 6종 반복 검사
.\DummyClient\x64\Release\DummyClient.exe --scenario malformed --clients 12 --duration 30 --csv DummyClient/results/malformed.csv

# 100명이 로그인한 뒤 10초마다 25% 강제 종료·재로그인
.\DummyClient\x64\Release\DummyClient.exe --scenario reconnect --clients 100 --fault-every-ms 10000 --disconnect-percent 25 --csv DummyClient/results/reconnect.csv

# 초당 10개 연결 교체; 매번 실제 로그인 수행
.\DummyClient\x64\Release\DummyClient.exe --scenario churn --clients 100 --churn-rate 10 --reconnect-delay-ms 1000 --csv DummyClient/results/churn.csv

# 프리셋은 서버 한도·계정 준비 후 실행
.\DummyClient\x64\Release\DummyClient.exe --preset lineage --csv DummyClient/results/lineage.csv
.\DummyClient\x64\Release\DummyClient.exe --preset stress --scenario mixed --csv DummyClient/results/mixed.csv

# 최초 접속 폭주: 클라이언트의 접속 속도 제한 해제
.\DummyClient\x64\Release\DummyClient.exe --scenario connect-only --clients 5000 --connect-rate 0 --duration 60
```

`churn`/`reconnect`는 Ready 연결에 장애를 주므로 로그인 실패가 계속되면 해당 장애 동작도 발생하지 않습니다. 실제 종료·재접속 횟수를 결과에서 확인하세요. 재접속 지연보다 서버의 기존 사용자 정리가 늦으면 `ALREADY_LOGGED_IN`이 계측될 수 있습니다.

## 설정

명시한 옵션은 위치에 상관없이 프리셋보다 우선합니다. 같은 옵션을 중복 지정하면 오류입니다.

| 프리셋 | 접속 수 | 접속 시도/초 | 실행 시간 | 전송 간격 | 발신자 비율 |
|---|---:|---:|---:|---:|---:|
| `smoke` (기본) | 100 | 100 | 60초 | 1,000ms | 100% |
| `lineage` | 5,000 | 250 | 120초 | 5,000ms | 1% |
| `stress` | 10,000 | 500 | 180초 | 1,000ms | 1% |

`lineage`의 이론적 정상 요청량은 약 10 chat TPS, 수신량은 초당 50,000개의 352바이트 브로드캐스트입니다. `stress`는 약 100 chat TPS, 초당 1,000,000개입니다. 재접속·지연·오류·실제 발신자 수에 따라 달라집니다.

| 옵션 | 기본값 / 의미 |
|---|---|
| `--host`, `--port` | `127.0.0.1`, `8694`; IPv4 주소 또는 IPv4로 해석 가능한 호스트 |
| `--local-ip` | `0.0.0.0`; 연결할 로컬 IPv4 인터페이스 선택 |
| `--clients` | `100`; 1~100,000개의 논리 클라이언트 슬롯 |
| `--duration` | `60`; 최초 접속 증가 구간을 포함한 총 초 수 |
| `--connect-rate` | `100`; 초기 접속과 재접속을 합친 초당 시도 제한. `0`이면 무제한 |
| `--interval-ms` | `1000`; 활성 클라이언트의 전송 시도 간격 |
| `--active-percent` | `100`; 전체 슬롯 중 발신자 비율, `0`이면 로그인만 유지. 올림한 인원을 슬롯에 분산 |
| `--timeout-ms` | `5000`; 접속·로그인·닉네임·채팅·비정상 패킷 응답 제한 시간 |
| `--reconnect-delay-ms` | `1000`; 연결 종료 후 재시도 대기. 전역 접속 속도 제한도 적용 |
| `--fault-every-ms` | `10000`; 강제 종료 파동 간격 |
| `--disconnect-percent` | `25`; 파동마다 현재 Ready 연결의 지정 비율(최소 1개)을 종료 |
| `--churn-rate` | `10`; 목표 연결 교체 수/초 |
| `--report-ms` | `1000`; 콘솔/CSV 기록 주기 |
| `--csv` | 미지정 시 콘솔만 출력. 지정 파일은 덮어쓰며 부모 폴더는 생성 |
| `--accounts` | 계정 TSV 경로. 미지정 시 아래 규칙으로 계정 문자열 생성 |
| `--id-prefix`, `--nickname-prefix` | `dummy_`, `dc_` |
| `--start-index` | `1`; 계정/닉네임 번호 시작점. 여러 부하 발생기는 서로 다른 범위를 지정 |
| `--password-env` | `DUMMY_PASSWORD`; 공통 비밀번호 환경 변수 이름 |

서버의 사용자별 초당 채팅 제한은 5회입니다. 200ms 미만 간격은 의도적으로 제한 오류를 유발할 수 있습니다. 클라이언트는 약 10ms 주기로 스케줄링하므로 더 짧은 간격을 정밀 보장하지 않습니다. 부하 발생기 자체가 지연되면 밀린 요청을 한꺼번에 보내지 않습니다.

## 결과 해석

- **Latency**: `steady_clock`으로 요청 제출 직전부터 대응 응답 처리까지 측정하는 RTT입니다. `connect`, `login`, `nickname`, `chat`, `ready`(접속 시작~닉네임까지 완료) 각각 표본 수·avg·p95·p99를 출력합니다. 서버 내부 처리 시간과 구분해야 합니다.
- 성공 응답만 해당 Latency 표본에 포함합니다. 오류·시간 초과는 별도 카운터에 남으므로 성공 Latency만 보고 장애를 판단하지 마세요. 표본 수가 0인 구간의 0ms는 측정 표본이 없다는 의미입니다.
- avg는 모든 성공 표본의 평균입니다. p95/p99는 고정 메모리 로그 히스토그램의 nearest-rank 근사값이며 버킷 상한을 표시합니다. 오차는 버킷 폭 기준 약 3.125% 이내입니다. 표본을 무제한 저장하지 않습니다.
- **`login_tps` / `chat_tps`**: 성공 로그인 응답 수/초, 본인이 보낸 채팅의 일치하는 브로드캐스트 수/초입니다. 런 고유 토큰·클라이언트 번호·증가 번호를 메시지에 넣습니다. 다른 사람의 메시지나 동일 응답의 중복 수신은 성공 TPS를 늘리지 않습니다.
- 한 클라이언트당 채팅 요청은 최대 1개만 미완료 상태로 둡니다. 현재 오류 응답에 요청 ID가 없기 때문입니다. 따라서 RTT가 전송 간격보다 커지면 실제 TPS가 줄고 `schedule_skipped`가 증가하는 **응답 대기형 부하**입니다. 무제한 요청 큐나 고정 TPS 보장 모델은 아닙니다.
- **처리량**: `tx/rx_packets_per_s`, `tx/rx_bytes_per_s`는 소켓 I/O 완료와 수신 프레임 파싱 기준입니다. 브로드캐스트 수신은 수신 처리량에 포함됩니다. TCP/IP 헤더·재전송량·서버 DB 처리량은 포함하지 않습니다. `tx_packets`에는 비정상 시험 전송도 포함됩니다.
- `sample` 행의 초당 수치는 직전 기록 이후 구간 값, `final` 행은 전체 실행 시간(접속 증가 포함) 평균입니다. 카운터와 Latency는 모두 누적 값입니다. CSV의 `login_result_N`, `nickname_result_N`, `chat_error_N`은 서버 enum 숫자별 결과입니다.
- `connected`는 TCP 연결, `ready`는 로그인과 필요한 닉네임 생성까지 완료한 연결입니다. `peak_*`는 최대 동시 수, `covered_clients`는 한 번 이상 목적 단계에 도달한 슬롯 수입니다. 연결만 수천 개 맺어졌다고 실제 로그인 동시 수용이 검증된 것은 아닙니다.
- `forced_disconnects`, `churn_disconnects`, `unexpected_disconnects`를 구분합니다. `abandoned_requests`는 장애/종료로 응답을 기다리던 요청이 중단된 수이며 timeout 카운터와 겹칠 수 있습니다. 의도한 강제 종료 중 중단된 요청은 성공 응답으로 처리하지 않습니다.
- 종료 시 대기 요청은 `shutdown_canceled`로 기록합니다. 종료 중 취소되는 I/O 완료를 모두 배출하고 `pending_io=0`을 출력합니다. 마지막 connected/ready는 종료 직전 값입니다. `max_tick_gap_ms`가 커지면 부하 발생기 자체의 CPU·수신 처리 병목도 확인하세요.

종료 코드는 `0`: 모든 슬롯이 한 번 이상 목적 단계에 도달하고 집계된 실패 없음, `1`: 미도달 슬롯이나 예상 밖 오류/timeout/인증·채팅 거부 있음, `2`: 옵션·초기화·출력 파일 오류, `130`: Ctrl+C/Break 중단입니다. 장애 시나리오에서 의도한 FIN/RST와 예상한 비정상 패킷 거부는 실패가 아닙니다. 코드 0은 설정된 동시 접속 SLA·목표 TPS 달성을 자동 보증하지 않으므로 peak·구간 지표·실제로 실행된 장애 횟수도 확인해야 합니다.

고접속수/Churn에서 로컬 임시 포트·TIME_WAIT·메모리 한계가 먼저 나타날 수 있습니다. 여러 프로세스/부하 발생기에 계정 범위와 `--local-ip`를 나누고, 실패 시 최종 `socket_error` 분포를 확인하세요. 단일 IOCP 처리 스레드가 전체 브로드캐스트를 소화하는 능력도 별도로 관찰해야 합니다.
