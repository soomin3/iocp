# IOCPServer 객체형 DB 구조 설명

## 1. 전체 호출 흐름

```text
CPacketDispatcher
  -> 등록된 IPacketHandler 객체 호출
  -> CLoginHandler::Handle
  -> Session의 로그인 Pending bool 설정
  -> SD_LOGIN_REQ 생성
  -> CDBManager::Submit
  -> DB 작업 큐 / DB 워커 스레드
  -> SD 패킷을 프로시저 입력으로 변환
  -> CStoredProcedure::Execute
  -> CProcedureBinder가 파라미터를 순서대로 바인딩
  -> CDBStatement가 SQL Server 프로시저 실행
  -> SELECT이면 CDBResultReader로 모든 행 조회
  -> DS_LOGIN_RES 생성
  -> IPacketSender가 DS 패킷을 IOCP 큐에 등록
  -> IOCP 워커의 CPacketDispatcher가 DS 패킷 처리
  -> sessionID로 세션을 찾고 generation과 Pending을 검증
  -> 상태 변경 및 응답
```

프로시저 객체 하나가 다음 정보를 모두 소유한다.

- 입력 파라미터
- 출력 파라미터
- 프로시저 반환값
- SELECT 결과 행
- 실행 성공 여부
- SQLSTATE, NativeError, ODBC 오류 메시지

## 2. 핵심 객체의 역할

### `IPacketHandler` / `CPacketDispatcher`

패킷 처리기는 `IPacketHandler`를 상속한 객체로 만든다. `CPacketDispatcher`는 `std::function`이나 람다 대신 등록된 처리기 객체의 주소를 보관하고 `Handle`을 호출한다. 처리기 객체의 수명은 `CServer`가 소유한다.

### `CAuthHandlers` / `CLoginHandler` / `CNickNameHandler`

`CAuthHandlers`는 CS 로그인/닉네임 처리기와 DS 로그인/닉네임 처리기를 소유하고 서버 시작 시 `RegisterAll`로 등록한다. CS 로그인 처리기는 Session의 Pending bool을 설정하고, 로그인 이후 처리기는 CUser에 Pending requestID를 저장한 뒤 SD 패킷을 제출한다. DS 로그인 처리기는 sessionID로 Session을 찾아 generation과 Pending을 검사하고, 로그인 이후 DS 처리기는 accountID로 `CUserManager`를 조회해 requestID를 검사한다.

### `CDBConnection`

ODBC 환경 핸들과 연결 핸들만 관리한다. 프로시저나 인증 타입을 알지 못한다.

### `CDBManager`

DB 작업 큐와 워커 스레드를 관리한다. 큐에는 프로시저 객체나 세션 주소가 아니라 직렬화된 `SD_*` `PacketBuffer`가 들어간다. DB 워커는 헤더의 packetID로 등록된 `IDBRequestHandler` 객체를 찾아 호출하며 세션과 유저 객체를 직접 변경하지 않는다.

### `IDBRequestHandler`

DB 워커에서 실행할 요청 처리기의 공통 인터페이스다. `CDBLoginRequestHandler`와 `CDBCreateNicknameRequestHandler`가 SD 패킷을 구체 타입으로 복사하고 프로시저 입력을 바인딩한 뒤 DS 응답을 만든다. 새 DB 요청은 DBManager의 분기 코드를 늘리지 않고 처리기 객체를 등록해 추가한다.

`Connect("DBConfig.txt", "TestDB")`는 이름이 `TestDB`인 설정을 선택해 접속한다. DB가 늘어나면 같은 파일에 한 줄씩 추가하고, 별도 `CDBManager` 인스턴스가 필요한 DB 이름을 선택하면 된다. DB 이름은 대소문자를 구분하며 중복을 허용하지 않는다. 빈 줄과 첫 번째 공백이 아닌 문자가 `;`인 줄은 주석으로 건너뛴다.

```text
; DATABASE = IP, PORT, ID, PASSWORD
TestDB = 127.0.0.1, 1433, game_user, game_password
LogDB = 127.0.0.1, 1433, log_user, log_password

; Windows 통합 인증은 ID와 PASSWORD를 모두 비워 둔다.
LocalDB = 127.0.0.1, 1433, ,
```

설정 파일은 서버의 현재 작업 폴더에 둔다. ID와 비밀번호는 둘 다 입력하거나 둘 다 비워야 하며, 둘 다 비우면 코드가 `Trusted_Connection=yes`를 사용한다. ODBC 드라이버명과 암호화 옵션은 `DBManager.cpp`에 있고 비밀번호를 포함한 연결 문자열 전체는 로그에 출력하지 않는다. 실제 설정 파일은 Git에서 제외한다.

### `SD_*` / `DS_*` 패킷

`SD_*`는 게임 서버가 DB 영역으로 보내는 요청이고 `DS_*`는 DB 영역이 게임 서버로 돌려주는 응답이다. CS/SC/SD/DS는 모두 하나의 `PacketID`에서 관리한다. 패킷 출처 검사를 통해 클라이언트 연결에서는 CS만, DB 응답 경로에서는 DS만 처리한다.

### DB 응답 대상 연결

별도 요청 추적 맵은 사용하지 않는다. 로그인 요청은 Session에 Pending bool을 설정하고 `SD_LOGIN_REQ`와 `DS_LOGIN_RES`가 sessionID와 generation을 전달한다. IOCP는 고정 세션 풀에서 대상을 찾고 현재 접속 generation과 비교한다. 로그인 이후 요청은 `CDBManager`가 발급한 requestID를 CUser가 하나만 Pending으로 보관하며, DS 패킷의 accountID로 현재 접속 User와 Session 문맥을 찾는다. 세션 포인터는 DB 경계를 넘지 않는다.

### `IPacketSender`

DB 워커가 만든 DS 패킷을 전송하는 유일한 인터페이스다. 현재 구현은 `PostQueuedCompletionStatus`로 게임 서버 IOCP에 패킷을 넣는다. DB를 별도 프로세스로 분리할 때는 DB 소켓의 송신 큐 구현으로 교체할 수 있다.

### `CDBStatement`

Statement 핸들을 자동으로 생성하고 해제한다. 저수준 파라미터/컬럼 바인딩, 실행, Fetch, 다음 결과 처리를 담당한다.

### `CProcedureBinder`

프로시저 객체의 파라미터를 등록된 순서대로 바인딩한다. 번호는 1부터 자동 증가한다. 프로시저명과 파라미터 개수로 ODBC 호출문도 자동 생성한다.

예를 들어 반환값 1개와 일반 파라미터 3개가 있으면 다음 문장을 만든다.

```sql
{? = CALL dbo.tp_UserLogin(?, ?, ?)}
```

### `CDBResultReader`

SELECT 컬럼을 순서대로 바인딩하고 `Fetch()`로 행을 읽는다. SQL Server 프로시저가 SELECT 전에 행 개수 결과를 만들더라도 실제 SELECT 결과 집합까지 이동한다.

### `CStoredProcedure`

모든 프로시저 객체의 실행 순서를 통일한다.

1. 이전 실행 상태와 SELECT 행 초기화
2. 파라미터 준비와 바인딩
3. 호출문 자동 생성
4. 프로시저 실행
5. SELECT 행 읽기
6. 남은 결과 처리
7. 출력 파라미터 완성
8. 프로시저별 결과 검증

## 3. 로그인 프로시저

선언만 보면 입출력 계약을 알 수 있다.

```cpp
class CProcedure_UserLogin final : public CStoredProcedure
{
public:
    TDBReturn<int32_t> ResultCode;
    TDBInputString<64> ID;
    TDBInputString<64> PW;
    TDBOutput<int64_t> AccountID;
    TDBOutputString<16> Nickname;

protected:
    const wchar_t* GetProcedureName() const override;
    bool BindParameters(CProcedureBinder& Binder) override;
    bool ValidateResult(std::wstring& ErrorMessage) override;
};
```

파라미터 순서는 `BindParameters`의 순서로 결정된다.

```cpp
bool CProcedure_UserLogin::BindParameters(CProcedureBinder& Binder)
{
    return
        Binder.Bind(ResultCode) &&
        Binder.Bind(ID) &&
        Binder.Bind(PW) &&
        Binder.Bind(AccountID) &&
        Binder.Bind(Nickname);
}
```

게임 서버는 프로시저 객체를 직접 만들지 않고 SD 패킷을 제출한다.

```cpp
const uint64_t Generation = Session.GetGeneration();
if (!Session.TryBeginLogin(Generation))
{
    return false;
}

SD_LOGIN_REQ Request;
Request.sessionID = Session.GetSessionID();
Request.generation = Generation;
// 검증된 ID/PW를 고정 길이 필드에 복사한다.
DBManager.Submit(Request);
```

DB 워커가 SD 패킷을 받은 뒤 `CProcedure_UserLogin`을 생성하고 실행한다. 결과는 프로시저 객체가 아니라 `DS_LOGIN_RES`에 복사해 IOCP로 보낸다. IOCP는 응답의 sessionID로 세션을 찾고, `CDSLoginHandler`는 Session이 Pending이며 generation이 현재 접속과 같을 때만 로그인을 완성한다.

인증 처리기는 서버가 객체로 소유하고 다음처럼 등록한다.

```cpp
CAuthHandlers AuthHandlers(DBManager, UserManager);
AuthHandlers.RegisterAll(PacketDispatcher);
```

각 인증 처리기가 DBManager 참조를 보관하므로 등록이나 DB 호출을 위한 람다는 필요하지 않다.

DB 전용 스레드나 테스트처럼 연결 객체를 직접 가진 곳에서는 동기로 실행할 수도 있다.

```cpp
CProcedure_UserLogin Proc;
Proc.ID = ID;
Proc.PW = PW;

const bool Executed = Proc.Execute(DBConnection);
```

IOCP 처리 스레드에서는 동기 실행하지 말고 SD 패킷을 `CDBManager::Submit`에 넣어야 한다.

## 4. 파라미터 객체

| 객체 | 의미 |
|---|---|
| `TDBReturn<T>` | 프로시저 반환값 |
| `TDBInput<T>` | 숫자 입력 파라미터 |
| `TDBOutput<T>` | 숫자 출력 파라미터 |
| `TDBInputOutput<T>` | 숫자 입출력 파라미터 |
| `TDBInputString<N>` | 최대 N자의 UTF-8 문자열 입력 |
| `TDBOutputString<N>` | 최대 N자의 UTF-8 문자열 출력 |
| `TDBInputOutputString<N>` | 최대 N자의 UTF-8 문자열 입출력 |
| `TDBColumn<T>` | SELECT 숫자 컬럼 |
| `TDBStringColumn<N>` | SELECT 문자열 컬럼 |

지원되는 기본 숫자 타입은 `int16_t`, `int32_t`, `int64_t`, `uint8_t`, `bool`, `float`, `double`이다.

입력값은 반드시 파라미터 객체에 대입해야 한다.

```cpp
Proc.AccountID = AccountID;
Proc.Nickname = Nickname;
```

다음처럼 내부 `Value`만 직접 바꾸면 입력 설정 여부가 기록되지 않으므로 사용하지 않는다.

```cpp
// 사용하지 말 것
Proc.AccountID.Value = AccountID;
```

NULL 입력은 명시적으로 설정한다.

```cpp
Proc.Nickname.SetNull();
```

출력과 컬럼의 NULL 여부는 다음처럼 확인한다.

```cpp
if (!Proc.AccountID.IsNull())
{
    Use(Proc.AccountID.Value);
}
```

## 5. 일반 프로시저 추가 예시

`dbo.tp_ChangeNickname`이 반환 코드, 계정 ID, 닉네임을 사용한다고 가정한다.

```cpp
class CProcedure_ChangeNickname final : public CStoredProcedure
{
public:
    TDBReturn<int32_t> ResultCode;
    TDBInput<int64_t> AccountID;
    TDBInputString<32> Nickname;

protected:
    const wchar_t* GetProcedureName() const override
    {
        return L"dbo.tp_ChangeNickname";
    }

    bool BindParameters(CProcedureBinder& Binder) override
    {
        return
            Binder.Bind(ResultCode) &&
            Binder.Bind(AccountID) &&
            Binder.Bind(Nickname);
    }
};
```

게임 서버에는 대응하는 SD/DS 패킷을 추가한다.

```cpp
struct SD_CHANGE_NICKNAME_REQ : PacketHeader
{
    uint64_t requestID;
    int64_t accountID;
    char nickname[64];
};

struct DS_CHANGE_NICKNAME_RES : PacketHeader
{
    uint64_t requestID;
    int64_t accountID;
    int32_t resultCode;
    uint8_t dbSucceeded;
};
```

새 DB 기능은 다음 순서로 추가한다.

1. `PacketList.inc`와 `Packet.h`에 SD/DS 계약 추가
2. 프로시저 객체 추가
3. 대응하는 `IDBRequestHandler` 객체를 구현하고 DBManager의 packetID 테이블에 등록
4. 로그인 이후 DS 응답에는 accountID를 돌려주고 User의 Pending requestID를 검증
5. DS 패킷 처리기 객체를 `CAuthHandlers`와 같은 도메인 처리기 묶음에 등록

`CDBConnection`, `CDBStatement`, `CProcedureBinder`, `CStoredProcedure`, `IOCPManager`의 전송 방식은 수정하지 않는다. DB를 별도 프로세스로 옮길 때도 SD/DS 계약과 게임 서버의 DS 처리기는 유지된다.

## 6. SELECT 프로시저 추가 예시

SELECT 한 행을 저장할 일반 구조체를 먼저 만든다.

```cpp
struct SInventoryRow
{
    int64_t ItemID = 0;
    int32_t Count = 0;
    std::string ItemName;
};
```

프로시저 객체는 입력과 전체 결과 행을 소유한다.

```cpp
class CProcedure_LoadInventory final : public CStoredProcedure
{
public:
    TDBInput<int64_t> AccountID;
    std::vector<SInventoryRow> Rows;

protected:
    const wchar_t* GetProcedureName() const override
    {
        return L"dbo.tp_LoadInventory";
    }

    bool BindParameters(CProcedureBinder& Binder) override
    {
        return Binder.Bind(AccountID);
    }

    void ResetResultRows() override
    {
        Rows.clear();
    }

    bool ReadResultRows(CDBResultReader& Reader) override
    {
        TDBColumn<int64_t> ItemID;
        TDBColumn<int32_t> Count;
        TDBStringColumn<64> ItemName;

        if (!Reader.Bind(ItemID) ||
            !Reader.Bind(Count) ||
            !Reader.Bind(ItemName))
        {
            return false;
        }

        while (Reader.Fetch())
        {
            if (ItemID.IsNull() || Count.IsNull())
            {
                return false;
            }

            Rows.push_back({
                ItemID.Value,
                Count.Value,
                ItemName.Value
            });
        }

        return Reader.Succeeded();
    }
};
```

실행이 끝나면 `Rows`를 반복한다.

```cpp
for (const SInventoryRow& Row : Proc.Rows)
{
    Use(Row);
}
```

결과가 매우 많을 수 있는 조회는 전부 메모리에 담지 말고 SQL 프로시저에 페이지 번호와 페이지 크기 입력을 추가하는 것이 좋다.

## 7. 실행 실패와 업무 실패

`Succeeded()`는 ODBC 실행과 결과 해석이 정상적으로 끝났는지를 뜻한다.

```cpp
if (!Proc.Succeeded())
{
    const SDBError& Error = Proc.GetError();

    // Error.Code
    // Error.Operation
    // Error.SqlState
    // Error.NativeError
    // Error.Message
}
```

`Succeeded() == true`여도 `ResultCode.Value != 0`일 수 있다. 이것은 DB 통신 실패가 아니라 프로시저가 정의한 로그인 거절, 중복 이름 등의 업무 결과다.

## 8. 현재 실행 정책

- DB 연결 1개와 DB 워커 1개를 사용한다.
- 여러 요청을 큐에 넣을 수 있지만 SQL은 한 번에 하나씩 실행된다.
- DB 워커는 SD 패킷을 프로시저로 실행하고 DS 패킷을 만든다.
- 현재 `IPacketSender`는 DS 패킷을 게임 서버 IOCP 큐에 넣는다.
- DS 처리는 IOCP 워커의 객체형 `IPacketHandler`에서 실행된다.
- 패킷 처리기는 `IPacketHandler` 객체로 등록되며 호출부는 람다 콜백을 소유하지 않는다.
- `StopWorker`는 큐에 들어간 작업을 모두 실행한 뒤 종료한다.
- 서버 종료 시 DB 워커를 먼저 비우고 IOCP 워커를 종료해야 DS 패킷이 유실되지 않는다.
- 병렬 DB 실행이 필요해지면 프로시저 객체는 그대로 두고 연결 풀과 워커 수만 확장하면 된다.

`Database.sql`은 로그인과 닉네임 생성 프로시저 계약을 제공한다. 로그인 프로시저는 반환값 `INT`, ID `NVARCHAR(64)`, PW `NVARCHAR(64)`, AccountID `BIGINT OUTPUT`, Nickname `NVARCHAR(16) OUTPUT`을 사용한다.
