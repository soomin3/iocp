# 게임 서버 검증 도구

`IOCPServer.slnx`의 별도 C++20 프로젝트에서 **서버 공통 테스트 11개**와 **게임 서버 전용 테스트 6개**를 함께 관리합니다. 공통 영역은 GameServer·DBServer의 IOCP·연결·메모리 수명을, 게임 서버 영역은 현재 구현된 로그인·닉네임·월드 채팅과 패킷 처리를 검증합니다.

- **VerificationWorkbench**: Windows 데스크톱 UI. 전체/서버 공통/게임 서버 실행 범위, 시나리오 선택, 반복 횟수, 실행·중지, 그룹별 결과와 실제 로그를 제공합니다.
- **LifetimeTests**: UI가 시나리오마다 실행하는 콘솔 테스트 프로세스. 실제 서버 소스를 직접 빌드해 사용합니다.

SQL Server나 DB 계정 없이 실행할 수 있습니다. 실제 루프백 소켓과 IOCP를 사용하며, DB 작업만 지연을 제어할 수 있는 모의 handler로 대체합니다. **SQL Server·OLE DB 쿼리 실행은 이 검증 범위에 포함되지 않습니다.**

## 실행

Visual Studio에서 `VerificationWorkbench`를 시작 프로젝트로 지정하고 **Debug / x64** 또는 **Release / x64**로 실행합니다. 종속 프로젝트인 `LifetimeTests`도 함께 빌드됩니다.

1. 실행 범위를 `전체`, `서버 공통`, `게임 서버` 중에서 고르고 체크박스로 항목을 선택합니다.
2. 반복 횟수(1~1000)를 지정하고 `범위 내 선택 실행` 또는 `범위 전체 실행`을 누릅니다.
3. 목록에서 PASS / FAIL / TIMEOUT / CANCELLED 상태와 소요 시간을 확인합니다.
4. `결과 폴더 열기`로 시나리오별 원본 로그, 항목별 `summary.tsv`, 그룹별 `group_summary.tsv`를 확인합니다.

목록에는 두 그룹이 항상 함께 보입니다. 범위를 바꿔도 체크 상태와 다른 그룹의 이전 결과는 유지하며, 아래 그룹 집계는 **이번 실행 대상**만 표시합니다.

검증은 화면과 별도 작업 스레드에서 순차 실행됩니다. 실패나 중지 결과를 통과로 처리하지 않습니다. 중지하거나 창을 닫으면 실행 중인 테스트 자식 프로세스를 Job Object로 종료·회수합니다. 이것은 **검증 도구의 취소**이며, 서버의 정상 종료 안전성은 각 테스트의 별도 검증 조건입니다.

게임 서버 전용 테스트는 **8694·8695**, 공통 테스트 중 `session_manager_shutdown`은 **8694**, `db_backend_shutdown`은 **8695** 포트를 사용합니다. 해당 포트에 GameServer·DBServer가 실행 중이면 먼저 직접 종료하세요. 도구는 기존 서버를 강제 종료하지 않으며 포트 충돌을 실패로 표시합니다. 다른 공통 테스트는 임시 루프백 포트를 사용합니다.

실행 파일과 결과 위치:

```text
x64/Debug/VerificationWorkbench.exe
x64/Debug/LifetimeTests.exe
x64/Debug/results/<실행 시각-프로세스 ID>/summary.tsv
x64/Debug/results/<실행 시각-프로세스 ID>/group_summary.tsv
x64/Debug/results/<실행 시각-프로세스 ID>/<시나리오>.log
```

Release는 `x64/Release/`입니다. UI는 최근 결과 폴더를 기억합니다. 전체 로그는 파일에 남기고 화면의 로그 길이는 제한합니다. 검증 소스는 Git 관리 대상이며 빌드 결과·실행 로그는 기존 출력 폴더 제외 규칙으로 관리됩니다.

## 서버 공통 테스트

여기서 공통은 연결·IOCP·수명처럼 서버 기반 기능에 관한 분류입니다. 실제 GameServer와 DBServer 구현을 검증하며 별도의 범용 서버 구현을 만들지는 않습니다.

| ID | 실행 상황 | 확인 조건 |
|---|---|---|
| `session_accept_cancel` | 대기 중 AcceptEx 취소, 성공 완료를 꺼낸 직후 종료 | 완료·재개 호출 회수 전 재사용 불가, 중복 완료 없음 |
| `session_queued_close` | READ 완료를 꺼낸 뒤 연결 종료 | 버퍼 보존, 한 번만 패킷 처리, CompleteIO 전 재사용 불가 |
| `session_send_close` | SEND_WAKE 대기 및 실제 WSASend 중 종료 | 소켓 종료만으로 송신 큐를 비우지 않음, 완료 회수 후 정리 |
| `session_resume_close` | 패킷 처리기를 이벤트로 멈춘 상태에서 종료 | 실행 중인 코루틴·수신 버퍼 보존, 처리 반환 후 정리 |
| `session_reuse` | 세션 재사용 후 이전 generation의 종료·사용자 분리 요청 | 다음 ACCEPT와 새 연결·사용자를 유지 |
| `session_parallel_close` | READ·WRITE를 등록한 뒤 4개 IOCP 워커와 송신·종료 경합 | 모든 완료 회수, pending=0, 중복 완료 없음 |
| `session_manager_shutdown` | 실제 IOCPManager의 수신 처리 중 Stop | 진행 중 처리와 대기 ACCEPT 회수 → 워커 join → 세션 풀 해제 |
| `db_cancel_connect` | AcceptEx·ConnectEx 취소와 소켓 종료 | 완료 전 코루틴 보존, 취소된 연결을 성공으로 게시하지 않음 |
| `db_cancel_io` | 수신 및 8 MiB 송신 중 취소·종료 | 코루틴 내부 버퍼 보존, 한 번만 재개·지역 객체 정리 |
| `db_late_completion` | 이미 도착한 수신 완료와 취소·시간 초과 | 완료 회수와 버퍼 보존, 취소·시간 초과 결과 우선 |
| `db_backend_shutdown` | 실제 DBClient·DBServer 종료와 늦은 모의 DB 결과 | 클라이언트 요청의 실패 완료, DB 작업의 요청·handler 보존 후 종료 |

의도적으로 완료를 보류하거나 이벤트로 처리기를 정지시켜 위험한 순서를 반복합니다. 부하 반복만으로 경합 발생을 추정하지 않습니다. 각 반복에는 20초 watchdog이 있으며, 이를 넘으면 별도 종료 코드 `3`으로 실패합니다. UI에는 TIMEOUT으로 표시됩니다.

## 게임 서버 전용 테스트

| ID | 실행 상황 | 확인 조건 |
|---|---|---|
| `game_login` | 빈 값·잘못된 자격, 정상·중복 로그인, 종료 후 재로그인 | 결과 코드, 기존 사용자·세션·generation 보존, 기존 연결 사용 가능 |
| `game_nickname` | 미인증 요청, 잘못된 UTF-8·길이, 중복·정상 이름, 재변경 | 입력·인증 오류, 중복 거부, 사용자 상태 반영, 재로그인 후 이름 유지 |
| `game_chat` | 게스트·이름 없는 사용자·정상 사용자의 채팅 | 수신 대상 제한, 발신자·본문·sequence·시각 일치, 잘못된 메시지 거부, 로그 쓰기 |
| `game_chat_limit` | 한 사용자가 6개 메시지 일괄 전송 | 5개 순서대로 전달, 6번째 RATE_LIMITED, 추가 전달·로그 없음 |
| `game_stale_response` | 닉네임 DB 응답 처리를 보류하고 같은 세션을 재사용해 재로그인 | 이전 응답 미전달, 새 사용자 인증·연결 보존, 진행 중 요청 교체 거부 |
| `game_protocol` | 분할·병합 요청, 클라이언트의 내부 SD/DS 패킷 위조, 잘못된 크기 | 정상 요청 조립·디스패치, 잘못된 연결 종료, 정상 연결은 계속 사용 가능 |

실제 `CIOCPManager`, 인증·채팅 처리기, `CWorldChat`, `CDBClient`, `CDBServer`를 조립해 실행합니다. 콘솔 입력을 기다리는 `CServer::Run()` 대신 테스트가 구성요소의 시작·종료를 제어하며, DB 쿼리 실행만 메모리 계정 데이터로 대체합니다. 감사 로그는 실제 파일에 쓰고 쓰기 완료 카운터와 오류를 확인합니다. 로그 파일은 `%TEMP%/IOCP-GameVerification-<프로세스 ID>/`에 남습니다.

지연 응답 시나리오는 다른 99개 슬롯을 점유해 **동일한 세션 객체의 generation 변경**을 확인합니다. 이미 생성된 닉네임을 가진 계정의 재접속 상태를 보호하는 시험이며, 새 닉네임 요청이 진행 중인 모든 순서까지 검증하는 것은 아닙니다. 패킷 분할 시험은 클라이언트 송신을 나누지만 커널의 수신 완료 분할 횟수를 강제하지는 않습니다.

## 명령줄과 AddressSanitizer

Developer PowerShell에서:

```powershell
msbuild .\LifetimeVerification\Workbench\VerificationWorkbench.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:PreferredToolArchitecture=x64
.\x64\Debug\LifetimeTests.exe --list
.\x64\Debug\LifetimeTests.exe --list --suite game
.\x64\Debug\LifetimeTests.exe --case session_reuse --repeat 100

# 전체 검증: 빌드 후 각 시나리오를 25회 실행하고 JSON 요약·로그 저장
.\LifetimeVerification\Run-All.ps1 -Configuration Debug -Repeat 25
.\LifetimeVerification\Run-All.ps1 -Configuration Release -Repeat 25

# 영역별 실행: summary.json과 groups.json에 결과 저장
.\LifetimeVerification\Run-All.ps1 -Configuration Debug -Suite common -Repeat 25
.\LifetimeVerification\Run-All.ps1 -Configuration Debug -Suite game -Repeat 25

# 별도 x64/ReleaseASan 출력 폴더 사용
.\LifetimeVerification\Run-All.ps1 -Configuration Release -Repeat 25 -Asan
```

이미 빌드했다면 `-SkipBuild`로 실행만 할 수 있습니다. 개별 테스트 종료 코드는 `0=통과`, `1=검증 실패`, `2=잘못된 인자`, `3=시간 초과`입니다. 실행 중 검증 조건이 깨지면 진행 중 커널 I/O를 소유한 스택을 임의로 풀지 않고 해당 테스트 프로세스를 실패로 종료합니다. UI가 다음 시나리오와 결과 수집을 담당합니다.

이 작업 환경에서는 MSVC `14.50.35717`과 x64 호스트 도구로 검증했습니다. 설치된 다른 툴체인에서 PDB 도구 버전 오류가 발생한다면 설치 상태를 확인하고, 해당 버전이 설치된 환경에서는 다음처럼 선택할 수 있습니다.

```powershell
.\LifetimeVerification\Run-All.ps1 -Configuration Release -Repeat 25 -Asan -ToolsVersion 14.50.35717
# UI/솔루션 MSBuild 명령에도 /p:VCToolsVersion=14.50.35717 사용 가능
```

ASan 실행에는 해당 MSVC의 런타임 DLL이 필요하며 `Run-All.ps1 -Asan`이 PATH에 추가합니다. ASan이 켜졌다고 모든 데이터 경합이 검출되는 것은 아닙니다. 잠금·소유권 코드 감사, 결정적인 실행 순서 테스트, 반복 시험과 함께 판단합니다.

UI 자체 자동 검증:

```powershell
.\x64\Debug\VerificationWorkbench.exe --smoke --snapshot "$PWD\x64\Debug\lifetime-ui.png"
.\x64\Debug\VerificationWorkbench.exe --smoke --suite game
.\x64\Debug\VerificationWorkbench.exe --smoke --suite common
.\x64\Debug\VerificationWorkbench.exe --smoke --cancel-after-ms 1
.\x64\Debug\VerificationWorkbench.exe --smoke --timeout-ms 1
```

`--smoke`는 실제 UI 실행 경로에서 지정한 범위의 시나리오를 한 번씩 수행하고 종료합니다. 범위 기본값은 `all`입니다. UI 종료 코드는 `0=통과`, `1=실패`, `2=중지`, `64=잘못된 인자`입니다. `--snapshot`은 도구 자신의 창만 PNG로 저장합니다.

## 감사 결과와 수정

**수정한 문제:** 이전 연결의 비동기 작업이 `GetGeneration()`을 확인한 뒤 별도 `Disconnect()` 또는 `DetachUser()`를 호출하던 경로에 검사·사용 사이 경합이 있었습니다.

```text
이전 연결의 작업이 generation 일치 확인
→ 다른 워커가 연결 종료·완료 회수·세션 재사용
→ 이전 작업이 새 연결을 종료하거나 새 사용자를 분리할 수 있음
```

`Disconnect(generation)`와 `DetachUser(generation)`가 같은 상태 잠금 안에서 조건과 변경을 처리하도록 추가하고 AuthHandlers·WorldChat의 지연 작업에 적용했습니다. 다음 수락 대기 중에는 generation 숫자가 아직 이전 연결과 같으므로, 조건부 종료는 `ACCEPTING` 상태도 거부합니다. IOCP 완료 처리와 서버 전체 종료의 무조건 종료 API는 유지했습니다.

**확인한 기존 보장:**

- GameServer의 pending I/O는 커널 대기뿐 아니라 완료 처리와 `Resume()` 반환까지 보존됩니다. `TryReset()`은 종료 중이고 pending=0이며 사용자가 분리된 뒤 프레임·큐를 정리합니다.
- DB 통신은 context당 단일 워커와 outstanding socket operation 하나를 전제로 합니다. 취소 요청은 즉시 프레임 파기를 의미하지 않으며, IOCP 완료를 회수한 뒤 정리합니다.
- DBServer는 네트워크 코루틴이 늦은 DB 결과를 회수하고 종료된 뒤 DB 작업 스레드를 join하고 IOCP를 닫습니다. 이 경로에서 추가 수명 결함은 발견하지 않았습니다.

**범위와 한계:** 이 검증은 현재 스레드 모델의 특정 위험 순서를 확인하며 모든 실행 순서의 안전성을 증명하지 않습니다. 실제 SQL/OLE DB 실행과 쿼리 취소는 대상 밖입니다. DBServer는 실행 중인 동기 DB handler가 반환할 때까지 기다리므로, 영구 정지한 DB 작업에 대한 종료 시간 상한은 보장하지 않습니다.

## 공통·게임 서버 통합 검증 결과

2026-10-20, Windows x64 / MSVC 14.50.35717에서 실행했습니다.

| 검증 | 결과 |
|---|---|
| 전체 솔루션 Debug / Release 빌드 | 통과 |
| 17개 시나리오 × 각각 25회 / Debug | 공통 275회 + 게임 서버 150회 통과 |
| 17개 시나리오 × 각각 25회 / Release | 공통 275회 + 게임 서버 150회 통과 |
| 17개 시나리오 × 각각 25회 / Release + AddressSanitizer | 공통 275회 + 게임 서버 150회 통과, ASan 오류 없음 |
| UI 전체 / 공통 / 게임 서버 실행 | 각각 17 / 11 / 6개 통과, 그룹별 파일 집계 일치 |
| UI 게임 서버 범위 중지 / 강제 시간 제한 | CANCELLED / TIMEOUT 및 종료 코드 2 / 1 확인, 공통 그룹 미실행 유지, 잔여 자식 프로세스 없음 |
| 명령줄 그룹 목록 / 범위 밖 case 지정 | 전체 17·공통 11·게임 6개, 범위 밖 실행 거부 |

**1,275회는 시나리오 반복 횟수이며 동시 접속 성능 수치가 아닙니다.** 결과 파일은 로컬 빌드 출력 폴더에 남습니다.

- Debug: `x64/Debug/results/cli-all-20261020-231054-044/summary.json`
- Release: `x64/Release/results/cli-all-20261020-231203-490/summary.json`
- ASan: `x64/ReleaseASan/results/cli-all-20261020-231259-293/summary.json`
- UI 전체: `x64/Debug/results/20261020-231426-398-21632/group_summary.tsv`

각 명령줄 결과 폴더의 `groups.json`에서도 공통·게임 서버 집계를 볼 수 있습니다.

## 수명 안전성 수정 당시 실행 결과

2026-10-20, Windows x64 / MSVC 14.50.35717 기준입니다.

| 검증 | 결과 |
|---|---|
| 전체 솔루션 Debug / Release 빌드 | 통과 |
| 새 11개 시나리오 × 각각 25회 / Debug | 275회 통과 |
| 새 11개 시나리오 × 각각 25회 / Release | 275회 통과 |
| 새 11개 시나리오 × 각각 25회 / Release + AddressSanitizer | 275회 통과, ASan 오류 없음 |
| UI 정상 실행 | 11/11 통과, 종료 코드 0, 요약·로그 저장 |
| UI 중지 / 활성 자식 실행 중 창 닫기 | CANCELLED, 종료 코드 2, 잔여 테스트 프로세스 없음 |
| UI 강제 시간 제한 / runner 누락 | TIMEOUT / FAIL, 종료 코드 1, 통과로 잘못 표시하지 않음 |
| 기존 Accept 네이티브 Debug / ASan 회귀 | 각각 500개 정상 연결 generation 통과; 4개 워커 경합 포함 |
| 기존 실제 GameServer + 모의 DB 회귀 | 100개 수락 슬롯, 101번째 대기 연결, FIN/RST·부분 헤더 384회, 로그인 320회, 종료 경합 6회 통과 |

새 시나리오의 **825회는 시나리오 반복 횟수이며 동시 접속 성능 수치가 아닙니다.** 기존 `GameServer/Verification`의 로컬 회귀 도구는 이전 제외 정책을 유지합니다. 새 프로젝트는 해당 제외 디렉터리에 의존하지 않으므로 새 checkout에서 빌드·실행할 수 있습니다.

이번 실행의 원본 결과는 로컬 출력 폴더에 저장되어 있습니다(빌드·실행 산출물이므로 Git 제외).

- `x64/Debug/results/cli-20261020-225848-031/summary.json`
- `x64/Release/results/cli-20261020-230132-151/summary.json`
- `x64/ReleaseASan/results/cli-20261020-230215-974/summary.json`
