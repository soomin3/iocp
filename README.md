# IOCP Game Server

Windows IOCP와 SQL Server 비동기 작업 큐를 결합한 C++20 게임 서버 프로젝트입니다.

클라이언트 접속부터 로그인, 닉네임 생성, 월드 채팅, JSONL 감사 로그까지 하나의 실행 흐름으로 구현했습니다. 네트워크·DB·게임 로직을 분리하고, 세션 재사용이나 지연된 비동기 완료처럼 IOCP 서버에서 자주 발생하는 동시성 문제를 방어하는 데 초점을 맞췄습니다.

## 주요 기능

- `AcceptEx`와 고정 세션 풀을 이용한 비동기 연결 수락
- IOCP 워커 기반 비동기 수신·송신 및 partial send 처리
- TCP 분할·병합을 고려한 패킷 누적 및 반복 디스패치
- 세션 generation을 이용한 이전 연결의 늦은 완료 무효화
- 객체형 패킷 처리기와 로그인 상태 검증
- SD/DS 내부 패킷을 사용하는 비동기 DB 요청·응답 파이프라인
- ODBC stored procedure 호출과 타입별 파라미터 바인딩
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
  → Session 수신 버퍼
  → PacketDispatcher
      ├─ 로그인/닉네임
      │   → SD 요청 패킷
      │   → DB 작업 큐와 전용 워커
      │   → ODBC stored procedure
      │   → DS 응답 패킷
      │   → IOCP completion queue
      │   → Session/User 상태 검증 후 응답
      └─ 월드 채팅
          → 사용자 상태·속도 제한 검사
          → 월드 채팅 큐
          → JSONL 로그 큐
          → 활성 사용자 브로드캐스트

응답 패킷
  → 세션별 단일 송신 큐
  → IOCP WRITE
  → partial-send offset 갱신
```

로그인 요청은 `sessionID + generation`으로 현재 연결을 식별합니다. 로그인 이후 DB 요청은 `accountID + requestID`를 사용합니다. 로그아웃이나 세션 재사용 이후 도착한 오래된 응답은 현재 상태와 일치하지 않으므로 적용되지 않습니다.

## 프로젝트 구조

```text
IOCPServer/
├─ Codes/
│  ├─ Database/   # DB 연결, 작업 큐, 프로시저 실행, 결과 처리
│  ├─ Function/   # 문자열 인코딩과 입력 검증
│  ├─ Game/       # 사용자, 월드 채팅, 채팅 로그
│  ├─ Handlers/   # 인증·채팅 패킷 처리기
│  ├─ Network/    # IOCP, 서버, 세션, 패킷 디스패처
│  ├─ main.cpp
│  └─ pch.cpp
├─ Headers/
│  ├─ Database/
│  ├─ Function/
│  ├─ Game/
│  ├─ Handlers/
│  ├─ Network/
│  └─ pch.h
└─ Resource/
   └─ PacketList.inc
```

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
- Microsoft ODBC Driver 18 for SQL Server

Developer PowerShell에서 다음 명령으로 빌드할 수 있습니다.

```powershell
msbuild .\IOCPServer.slnx /t:Build /p:Configuration=Debug /p:Platform=x64
msbuild .\IOCPServer.slnx /t:Build /p:Configuration=Release /p:Platform=x64
```

프로젝트는 `/W4`, `/WX`, C++20, UTF-8 컴파일 옵션을 사용합니다.

## DB 설정

서버는 실행 작업 디렉터리의 `DBConfig.txt`에서 `TestDB` 항목을 읽습니다. 이 파일은 계정 정보를 포함할 수 있으므로 Git에서 제외됩니다.

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

구체적인 파라미터 순서와 타입은 [AuthProcedures.cpp](IOCPServer/Codes/Database/AuthProcedures.cpp)에서 확인할 수 있습니다.

## 실행

저장소 루트에 `DBConfig.txt`를 준비한 뒤 같은 위치에서 실행합니다.

```powershell
.\x64\Debug\IOCPServer.exe
```

서버는 기본적으로 TCP `8694` 포트를 사용하고 채팅 로그를 `logs/`에 기록합니다.

실행 중 사용할 수 있는 명령은 다음과 같습니다.

| 명령 | 설명 |
|---|---|
| `help` | 명령 목록 출력 |
| `status` | 세션, pending I/O, DB·채팅·로그 큐와 처리 지표 출력 |
| `quit` | 작업 큐를 배출하고 서버 정상 종료 |

## 동시성 안전장치

- 커널에 제출한 I/O마다 pending 수를 한 번 증가시키고 완료 시 한 번 감소시킵니다.
- 세션은 disconnect 중이며 pending I/O와 연결된 사용자가 모두 없을 때만 재사용합니다.
- 한 세션에는 READ와 WRITE를 각각 하나만 outstanding 상태로 유지합니다.
- 송신 큐는 partial send offset을 보존하며 최대 1 MiB로 제한합니다.
- 로그인 DS 응답은 session ID, generation, pending 상태가 모두 일치할 때만 반영합니다.
- 로그인 이후 DS 응답은 현재 User의 request ID와 일치할 때만 반영합니다.
- 공유 컬렉션 잠금을 보유한 채 소켓 송신이나 파일 I/O를 수행하지 않습니다.
- 종료 시 DB 큐, 네트워크, 채팅 큐, 로그 큐 순서로 작업을 정리합니다.

## 현재 제한 사항

- 클라이언트 전송 구간에 암호화가 없어 로컬 학습·포트폴리오 용도를 전제로 합니다.
- 패킷은 Windows/MSVC 구조체 레이아웃과 little-endian 환경을 전제로 합니다.
- DB 연결과 워커가 각각 하나이므로 SQL 작업은 큐 순서대로 실행됩니다.
- 채팅 로그는 프로세스 로컬 파일이며 다중 서버 환경의 중앙 로그 수집은 구현하지 않았습니다.
- 실제 서비스에서는 비밀번호를 평문이나 복호화 가능한 형태로 저장하지 않고 안전한 비밀번호 해시 정책을 적용해야 합니다.
