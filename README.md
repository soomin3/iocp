# IOCP Game Server

Windows IOCP 게임 서버와 SQL Server 전용 DB 서버를 TCP로 연결한 C++20 프로젝트입니다.

클라이언트 접속부터 로그인, 닉네임 생성, 월드 채팅, JSONL 감사 로그까지 하나의 실행 흐름으로 구현했습니다. 네트워크·DB·게임 로직을 분리하고, 세션 재사용이나 지연된 비동기 완료처럼 IOCP 서버에서 자주 발생하는 동시성 문제를 방어하는 데 초점을 맞췄습니다.

## 주요 기능

- `AcceptEx` 코루틴과 고정 세션 풀을 이용한 비동기 연결 수락
- IOCP 워커 기반 비동기 수신·송신 및 partial send 처리
- C++20 코루틴으로 표현한 세션별 수신·송신 루프
- TCP 분할·병합을 고려한 패킷 누적 및 반복 디스패치
- 세션 generation을 이용한 이전 연결의 늦은 완료 무효화
- 객체형 패킷 처리기와 로그인 상태 검증
- SD/DS 내부 패킷을 사용하는 비동기 DB 요청·응답 파이프라인
- `ConnectEx`·`AcceptEx`와 IOCP 코루틴 기반 서버 간 TCP 통신
- 별도 DBServer의 OLE DB stored procedure 호출과 타입별 파라미터 바인딩
- 중복 로그인 차단과 닉네임 유일성 처리
- 순차 처리되는 월드 채팅과 활성 사용자 브로드캐스트
- 사용자별 초당 5회 채팅 제한
- 세션 송신 큐, DB 큐, 채팅 큐, 로그 큐의 용량 제한
- 일자별 UTF-8 JSONL 채팅 로그
- `help`, `status`, `quit` 콘솔 명령과 정상 종료 절차

## 처리 구조

```text
TCP bytes
  → IOCP READ
  → Session 수신 코루틴 재개
  → Session 수신 버퍼
  → PacketDispatcher
      ├─ 로그인/닉네임
      │   → SD 요청 패킷
      │   → GameServer DB 요청 큐와 전용 IOCP 코루틴
      │   → TCP 127.0.0.1:8695 → DBServer IOCP 코루틴
      │   → 별도 DB 작업 스레드 → OLE DB stored procedure
      │   → DS 응답 패킷
      │   → TCP 응답 → GameServer IOCP completion queue
      │   → Session/User 상태 검증 후 응답
      └─ 월드 채팅
          → 사용자 상태·속도 제한 검사
          → 월드 채팅 큐
          → JSONL 로그 큐
          → 활성 사용자 브로드캐스트

응답 패킷
  → 세션별 단일 송신 큐
  → 대기 중인 송신 코루틴을 IOCP SEND_WAKE로 재개
  → IOCP WRITE
  → 송신 코루틴 재개 및 partial-send offset 갱신
```

로그인 요청은 `sessionID + generation`으로 현재 연결을 식별합니다. 로그인 이후 DB 요청은 `accountID + requestID`를 사용합니다. 로그아웃이나 세션 재사용 이후 도착한 오래된 응답은 현재 상태와 일치하지 않으므로 적용되지 않습니다.

`GameServer`는 세션마다 수락 코루틴을 시작해 `AcceptEx` 완료를 기다립니다. 기존 IOCP 워커에서 재개된 코루틴이 수락 소켓의 컨텍스트 갱신과 IOCP 등록을 마친 뒤 수신·송신 루프를 시작합니다. 즉시 성공도 IOCP 완료에서 한 번만 재개하고, 실패·취소 완료는 코루틴을 종료합니다. 서버 종료 시 새 수락 제출을 차단하고 대기 중인 수락을 취소하며, 완료 처리와 워커 종료 후 리슨 소켓을 닫습니다.

수신 루프는 `co_await`로 `WSARecv` 완료를 기다리고, 기존 IOCP 워커에서 재개되어 패킷을 처리한 뒤 다음 수신을 제출합니다. 즉시 성공한 수신도 IOCP 완료에서 한 번만 재개하며, 실패·취소 완료와 EOF는 수신 루프를 종료합니다. 수락·수신·송신 코루틴 프레임은 완료 처리와 재개 호출을 포함한 pending I/O가 모두 정리된 뒤 파괴합니다.

송신 루프도 세션마다 코루틴 하나를 유지합니다. 큐가 비면 중단하고, 새 데이터가 들어오면 `SEND_WAKE` 알림을 IOCP에 게시해 재개합니다. `Send()`는 큐 등록 결과를 반환하며 실제 `WSASend`와 완료 처리는 IOCP 워커에서 수행합니다. 부분 송신은 남은 바이트부터 이어 보내고, 실패·취소 완료는 연결을 종료합니다. 연결 종료 후 pending I/O가 모두 정리되면 종료된 프레임과 빈 큐에서 대기 중인 프레임을 모두 회수합니다.

서버 간 통신은 양쪽 프로세스의 전용 IOCP 워커에서 `ConnectEx`·`AcceptEx`·`WSASend`·`WSARecv`를 기다리는 코루틴으로 처리합니다. 요청 하나의 응답을 받은 뒤 다음 요청을 보내는 순서를 유지하며, 부분 송수신은 같은 프레임의 남은 바이트부터 이어갑니다. timeout이나 종료 시에는 I/O 취소를 요청하고 완료 통지를 회수한 뒤 버퍼와 코루틴 프레임을 해제합니다. DBServer의 COM 초기화, OLE DB 연결·호출·해제는 별도 DB 작업 스레드에서 수행하고 결과를 IOCP에 알립니다.

## 프로젝트 구조

솔루션에는 서버·클라이언트 실행 프로젝트 4개와 검증용 실행 프로젝트 2개가 있습니다.

```text
IOCPServer.slnx
├─ GameServer/    # 기존 IOCPServer 프로젝트
├─ LoginServer/   # 로그인 서버 분리용 실행 프로젝트
├─ DBServer/      # OLE DB 연결·프로시저 실행 전용 서버
├─ DummyClient/   # IOCP 부하·장애 테스트 클라이언트
├─ LifetimeVerification/ # 서버 공통·게임 기능 테스트와 Windows 데스크톱 검증 UI
└─ Common/        # 공통 패킷 계약과 서버 간 TCP 프레임 처리
```

`GameServer`는 클라이언트 인증 상태·게임 로직과 DB 요청 큐를 담당하고, `DBServer`만 SQL Server에 접속합니다. 두 프로세스는 기존 SD/DS 패킷을 TCP로 교환합니다. `LoginServer`는 아직 최소 진입점이며 로그인 상태 처리는 `GameServer`에 남아 있습니다.

`DummyClient`는 대량 접속, 실제 로그인·닉네임 생성, 주기적 채팅, RTT(avg/p95/p99), TPS·처리량, 강제 재접속, Connection Churn, 비정상 패킷 검증을 지원합니다. [빌드·시나리오·실행 방법](DummyClient/README.md)을 참고하세요. 5,000/10,000명 프리셋이 있지만 현재 `GameServer`의 세션 풀 한도는 100이므로 대규모 로그인 시험 전 서버 한도와 테스트 계정을 준비해야 합니다.

`VerificationWorkbench`를 시작 프로젝트로 지정하면 **서버 공통 11개**(연결 종료·IOCP 완료·수명)와 **게임 서버 전용 6개**(로그인·닉네임·채팅·횟수 제한·지연 DB 응답·패킷 경계) 테스트를 함께 관리할 수 있습니다. 전체 또는 영역별로 선택·반복 실행하고 그룹별 결과와 로그를 확인합니다. 실제 서버 소스를 사용하는 `LifetimeTests`가 함께 빌드됩니다. 실제 루프백 IOCP와 모의 DB를 사용하며 SQL Server·OLE DB 실행은 포함하지 않습니다. [검증 시나리오·실행·감사 결과](LifetimeVerification/README.md)를 참고하세요.

`GameServer` 내부 구성은 다음과 같습니다.

```text
GameServer/
├─ Codes/
│  ├─ Function/   # 문자열 인코딩과 입력 검증
│  ├─ Game/       # 사용자, 월드 채팅, 채팅 로그
│  ├─ Handlers/   # 인증·채팅 패킷 처리기
│  ├─ Network/    # IOCP, 세션, 패킷 디스패처, DBClient
│  ├─ main.cpp
│  └─ pch.cpp
└─ Headers/
   ├─ Function/
   ├─ Game/
   ├─ Handlers/
   ├─ Network/
   └─ pch.h
```

패킷 정의는 `Common/Protocol/`, 서버 간 패킷 크기와 통신 상수는 `Common/Network/DBTransport.h`, IOCP 코루틴과 TCP 프레임 송수신은 `Common/Network/DBIocp.h`에 있습니다. DB 설정 예제와 SQL 스크립트는 `DBServer/Resource/`에 있습니다.

## 패킷 계약

모든 패킷은 4바이트 헤더로 시작합니다.

```cpp
struct PacketHeader
{
    uint16_t packetSize;
    PacketID packetID;
};
```

| ID | 패킷 | 크기 | 역할 |
|---:|---|---:|---|
| 1 | `CS_LOGIN_REQ` | 68 | 로그인 요청 |
| 2 | `SC_LOGIN_RES` | 8 | 로그인 결과 |
| 3 | `CS_CREATE_NICKNAME_REQ` | 68 | 닉네임 생성 요청 |
| 4 | `SC_CREATE_NICKNAME_RES` | 6 | 닉네임 생성 결과 |
| 5 | `CS_CHAT_REQ` | 260 | 월드 채팅 요청 |
| 6 | `SC_CHAT_BROADCAST` | 352 | 월드 채팅 브로드캐스트 |
| 7 | `SC_CHAT_ERROR` | 8 | 채팅 오류 응답 |
| 8 | `SD_LOGIN_REQ` | 80 | 서버 내부 로그인 DB 요청 |
| 9 | `DS_LOGIN_RES` | 104 | 서버 내부 로그인 DB 응답 |
| 10 | `SD_CREATE_NICKNAME_REQ` | 88 | 서버 내부 닉네임 DB 요청 |
| 11 | `DS_CREATE_NICKNAME_RES` | 96 | 서버 내부 닉네임 DB 응답 |

클라이언트 연결에서는 CS 패킷만 허용합니다. SD/DS 패킷이 클라이언트 소켓에서 수신되면 내부 패킷 위조로 간주해 연결을 종료합니다. 문자열 필드는 UTF-8이며 SQL Server의 `NVARCHAR` 경계에서만 UTF-16으로 변환합니다.

## 빌드 환경

- Windows 10/11
- Visual Studio C++ 데스크톱 개발 도구와 v145 toolset
- Windows SDK
- SQL Server
- Microsoft OLE DB Driver 19 for SQL Server (`MSOLEDBSQL19`)
- Visual Studio C++ ATL 구성 요소

Developer PowerShell에서 다음 명령으로 빌드할 수 있습니다.

```powershell
msbuild .\IOCPServer.slnx /t:Build /p:Configuration=Debug /p:Platform=x64
msbuild .\IOCPServer.slnx /t:Build /p:Configuration=Release /p:Platform=x64
```

`GameServer`와 `DBServer` 프로젝트는 `/W4`, `/WX`, C++20, UTF-8 컴파일 옵션을 사용합니다. `GameServer`는 ODBC/OLE DB 라이브러리를 링크하지 않습니다.

## DB 설정

`DBServer`는 실행 작업 디렉터리의 `DBConfig.txt`에서 `TestDB` 항목을 읽습니다. `GameServer`에는 DB 계정 설정이 필요 없습니다. 이 파일은 계정 정보를 포함할 수 있으므로 Git에서 제외됩니다.

```text
; DATABASE = IP, PORT, ID, PASSWORD
TestDB = 127.0.0.1, 1433, game_user, game_password
```

Windows 통합 인증을 사용할 때는 ID와 비밀번호를 모두 비웁니다.

```text
TestDB = 127.0.0.1, 1433, ,
```

현재 인증 흐름은 다음 SQL Server 프로시저 계약을 전제로 합니다.

- `dbo.tp_UserLogin`: 반환 코드, ID, PW, AccountID 출력, Nickname 출력
- `dbo.tp_UserCreateNickname`: 반환 코드, AccountID, Nickname

프로시저는 기존 반환 코드와 입출력 파라미터 계약을 유지합니다. 문자열은 SQL 경계에서 UTF-8과 UTF-16으로 변환합니다.

OLE DB의 명령 제한 시간은 3초이며, 오류가 발생한 SQL 명령은 재실행하지 않습니다. SQL Server 연결 자체가 끊어졌다면 DBServer를 재시작해야 합니다.

## 실행

저장소 루트에 `DBConfig.txt`를 준비한 뒤 **DBServer를 먼저 실행**합니다. 이어서 별도 터미널에서 GameServer를 실행합니다.

```powershell
.\x64\Debug\DBServer.exe
# 별도 터미널
.\x64\Debug\GameServer.exe
```

GameServer는 클라이언트용 TCP `8694` 포트를 사용하고 채팅 로그를 `logs/`에 기록합니다. DBServer는 로컬 전용 `127.0.0.1:8695`에서 하나의 GameServer 연결을 처리합니다. 다른 호스트로의 배포나 여러 GameServer 연결은 현재 지원하지 않습니다.

GameServer 시작 시 DBServer에 연결할 수 없으면 초기화에 실패합니다. 실행 중 연결 장애·5초 응답 제한 초과·잘못된 패킷은 해당 요청을 DB 오류로 완료합니다. 이후 요청에서 1초 간격 제한으로 재접속을 시도하며, 실행 여부가 불확실한 요청을 자동 재전송하지 않습니다. 닉네임 변경이 DB에 반영된 뒤 응답만 유실됐다면 재로그인으로 저장된 상태를 다시 읽을 수 있습니다.

서버 간 연결 시도는 1초, 프레임 송신과 응답 수신은 각각 5초의 제한을 적용합니다. 분할 데이터가 도착해도 기한을 연장하지 않습니다. DBServer의 요청 수신 대기는 첫 바이트가 올 때까지 유지하며, 첫 바이트 이후에는 프레임 전체를 5초 안에 받아야 합니다. DBServer 종료 시 진행 중인 DB 작업의 결과까지 회수한 뒤 스레드를 종료합니다.

실행 중 사용할 수 있는 명령은 다음과 같습니다.

| 명령 | 설명 |
|---|---|
| `help` | 명령 목록 출력 |
| `status` | 세션, pending I/O, DB·채팅·로그 큐와 처리 지표 출력 |
| `quit` | DB 대기 요청을 실패로 완료하고 서버 정상 종료 |

## 동시성 안전장치

- 커널 I/O와 IOCP 송신 재개 알림마다 pending 수를 한 번 증가시키고 완료 처리와 코루틴 재개가 끝나면 한 번 감소시킵니다.
- 세션은 disconnect 중이며 pending I/O와 연결된 사용자가 모두 없을 때만 재사용합니다.
- 한 세션에는 READ와 WRITE를 각각 하나만 outstanding 상태로 유지합니다.
- 송신 큐는 partial send offset을 보존하며 최대 1 MiB로 제한합니다.
- 로그인 DS 응답은 session ID, generation, pending 상태가 모두 일치할 때만 반영합니다.
- 로그인 이후 DS 응답은 현재 User의 request ID와 일치할 때만 반영합니다.
- 지연된 DB 응답·브로드캐스트의 종료 요청은 generation 확인과 종료를 같은 잠금 안에서 처리하여 재사용된 연결을 보호합니다.
- 공유 컬렉션 잠금을 보유한 채 소켓 송신이나 파일 I/O를 수행하지 않습니다.
- 종료 시 DB 통신을 중단하고 미완료 요청에 실패 응답을 게시한 뒤 IOCP를 종료합니다.

## 현재 제한 사항

- 클라이언트 전송 구간에 암호화가 없어 로컬 학습·포트폴리오 용도를 전제로 합니다.
- 패킷은 Windows/MSVC 구조체 레이아웃과 little-endian 환경을 전제로 합니다.
- 서버 간 통신은 프로세스마다 IOCP 워커 하나를 사용하며, DBServer의 DB 연결과 DB 작업 스레드도 하나이므로 요청을 순차 처리합니다. 서버 간 통신은 로컬 신뢰 프로세스를 전제로 하며 별도 인증·암호화는 없습니다.
- 채팅 로그는 프로세스 로컬 파일이며 다중 서버 환경의 중앙 로그 수집은 구현하지 않았습니다.
- 실제 서비스에서는 비밀번호를 평문이나 복호화 가능한 형태로 저장하지 않고 안전한 비밀번호 해시 정책을 적용해야 합니다.
