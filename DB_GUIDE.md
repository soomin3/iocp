# IOCPServer DB 코드 설명과 프로시저 추가 방법

## 1. 현재 구조

DB 호출 흐름은 아래 순서다.

```text
패킷 핸들러
  -> CDBManager::ExecuteProcedureAsync
  -> DB 작업 큐
  -> DB 전용 워커 스레드
  -> 프로시저 객체의 Execute
  -> CStoredProcedure::ExecuteProcedure
  -> CDBStatement(ODBC)
  -> SQL Server 저장 프로시저
  -> 결과 콜백
```

- `DBManager.h/.cpp`
  - SQL Server 연결과 DB 워커 스레드를 관리한다.
  - 하나의 ODBC 연결은 하나의 워커 스레드에서만 사용한다.
  - `ExecuteProcedureAsync`는 결과 타입과 관계없이 모든 프로시저 객체를 큐에 넣을 수 있다.
- `DBStatement.h/.cpp`
  - ODBC Statement 핸들을 생성하고 소멸할 때 자동으로 해제한다.
  - 숫자/문자열 입력·출력 파라미터 바인딩, 결과 컬럼 바인딩, 실행, `Fetch`, 다음 결과 처리를 담당한다.
  - ODBC 실패 시 SQLSTATE, NativeError, 메시지를 콘솔에 출력한다.
- `StoredProcedure.h/.cpp`
  - 모든 저장 프로시저가 공통으로 사용하는 실행 순서를 담당한다.
  - Statement 생성 -> 파라미터 바인딩 -> 실행 -> 결과 행 읽기 -> 남은 결과 소진 순으로 처리한다.
  - UTF-8 패킷 문자열을 SQL Server용 UTF-16 문자열로 바꾸는 함수도 제공한다.
- `AuthProcedureTypes.h`
  - 인증 프로시저가 반환하는 C++ 결과 구조체를 둔다.
- `AuthProcedures.h/.cpp`
  - 인증 도메인의 실제 저장 프로시저별 파라미터와 결과 해석만 둔다.

## 2. 로그인 프로시저가 동작하는 방법

`CProcedure_UserLogin` 객체는 ID와 PW를 값으로 보관한다. 비동기 큐에서 실제 실행될 때까지 문자열 수명이 유지되므로 ODBC가 이미 사라진 메모리를 참조하지 않는다.

현재 호출 계약은 다음과 같다.

```sql
{? = CALL dbo.tp_UserLogin(?, ?, ?)}
```

| 순서 | 방향 | SQL 타입 | 의미 |
|---:|---|---|---|
| 1 | 반환값 | `INT` | 0이면 로그인 성공, 그 외는 업무 실패 코드 |
| 2 | 입력 | `NVARCHAR(64)` | ID |
| 3 | 입력 | `NVARCHAR(64)` | PW |
| 4 | 출력 | `BIGINT` | 성공한 계정 ID |

ODBC 출력 파라미터는 프로시저가 만든 모든 결과를 처리한 뒤 확정된다. `CStoredProcedure::ExecuteProcedure`가 내부에서 `SQLMoreResults`를 끝까지 호출하므로 각 프로시저에서 이 반복문을 다시 작성할 필요가 없다.

`SLoginProcedureResult::Executed`는 "DB 호출과 결과 해석까지 정상 완료했는가"를 뜻한다. `Executed == true`여도 `ResultCode != 0`이면 DB 통신 실패가 아니라 로그인 거절 같은 업무 실패다.

## 3. 새 프로시저 추가 규칙

새 프로시저는 아래 세 가지만 만들면 된다.

1. 반환할 결과 구조체
2. 입력값을 보관하고 `ResultType`과 `Execute`를 제공하는 프로시저 클래스
3. `Execute` 안의 호출 문자열, 바인딩 순서, 결과 검증

`CDBManager`에는 프로시저별 실행 함수를 추가할 필요가 없다. 호출 위치에서 `ExecuteProcedureAsync`에 프로시저 객체와 콜백을 넘기면 된다.

### 출력 파라미터 프로시저 예시

다음 예시는 `dbo.tp_ChangeNickname`이 반환값, 계정 ID, 새 닉네임을 받는다고 가정한 템플릿이다.

```cpp
struct SChangeNicknameResult
{
    bool Executed = false;
    int32_t ResultCode = -1;
};

class CProcedure_ChangeNickname : public CStoredProcedure
{
public:
    using ResultType = SChangeNicknameResult;

    CProcedure_ChangeNickname(int64_t _AccountID, std::string _Nickname)
        : m_AccountID(_AccountID), m_Nickname(std::move(_Nickname))
    {
    }

    ResultType Execute(SQLHDBC _ConnectionHandle);

private:
    int64_t m_AccountID;
    std::string m_Nickname;
};
```

```cpp
CProcedure_ChangeNickname::ResultType CProcedure_ChangeNickname::Execute(
    SQLHDBC _ConnectionHandle)
{
    ResultType Result;
    std::wstring Nickname;

    if (!Utf8ToWide(m_Nickname, Nickname))
    {
        return Result;
    }

    int32_t ResultCode = -1;
    SQLLEN ResultCodeIndicator = 0;
    SQLLEN AccountIDIndicator = 0;
    SQLLEN NicknameIndicator = SQL_NTS;

    const bool Executed = ExecuteProcedure(
        _ConnectionHandle,
        L"{? = CALL dbo.tp_ChangeNickname(?, ?)}",
        [&](CDBStatement& _Statement)
        {
            return
                _Statement.BindOutput(1, ResultCode, ResultCodeIndicator) &&
                _Statement.BindInput(2, m_AccountID, AccountIDIndicator) &&
                _Statement.BindInputWString(3, Nickname, 32, NicknameIndicator);
        });

    if (!Executed || ResultCodeIndicator == SQL_NULL_DATA)
    {
        return Result;
    }

    Result.Executed = true;
    Result.ResultCode = ResultCode;
    return Result;
}
```

호출 코드는 다음 형태다.

```cpp
m_DBManager.ExecuteProcedureAsync(
    CProcedure_ChangeNickname(AccountID, std::move(Nickname)),
    [](SChangeNicknameResult _Result)
    {
        // 결과에 맞는 서버 처리를 한다.
    });
```

### SELECT 결과 행을 반환하는 프로시저 예시

결과 행이 있으면 `ExecuteProcedure`의 네 번째 인자로 결과 읽기 함수를 전달한다.

```cpp
const bool Executed = ExecuteProcedure(
    _ConnectionHandle,
    L"{CALL dbo.tp_LoadInventory(?)}",
    [&](CDBStatement& _Statement)
    {
        return _Statement.BindInput(1, m_AccountID, AccountIDIndicator);
    },
    [&](CDBStatement& _Statement)
    {
        int64_t ItemID = 0;
        int32_t Count = 0;
        SQLLEN ItemIDIndicator = 0;
        SQLLEN CountIndicator = 0;

        if (!_Statement.BindColumn(1, ItemID, ItemIDIndicator) ||
            !_Statement.BindColumn(2, Count, CountIndicator))
        {
            return false;
        }

        while (true)
        {
            const SQLRETURN FetchResult = _Statement.Fetch();

            if (FetchResult == SQL_NO_DATA)
            {
                return true;
            }

            if (!SQL_SUCCEEDED(FetchResult))
            {
                return false;
            }

            // NULL 가능 컬럼은 Indicator를 먼저 검사한다.
            Result.Items.push_back({ ItemID, Count });
        }
    });
```

SELECT 결과를 반환하는 SQL Server 프로시저에는 `SET NOCOUNT ON`을 넣는 것이 좋다. SELECT보다 먼저 실행된 INSERT/UPDATE 행 개수 결과가 섞이지 않아 첫 결과 집합을 바로 읽을 수 있다.

## 4. 바인딩 규칙

현재 바로 사용할 수 있는 스칼라 타입은 다음과 같다.

| C++ 타입 | SQL 타입 |
|---|---|
| `int16_t` | `SMALLINT` |
| `int32_t` | `INT` |
| `int64_t` | `BIGINT` |
| `uint8_t` | `TINYINT` |
| `bool` | `BIT` |
| `float` | `REAL` |
| `double` | `DOUBLE` |

- 입력 숫자: `BindInput`
- 출력 숫자: `BindOutput`
- 입력 문자열: `BindInputWString`
- 출력 문자열: `BindOutputWString`
- SELECT 숫자 컬럼: `BindColumn`
- SELECT 문자열 컬럼: `BindColumnWString`

파라미터 번호는 ODBC 호출 문자열의 `?` 순서와 정확히 같아야 한다. `{? = CALL ...}` 형태에서는 반환값이 1번이고 첫 프로시저 인자가 2번이다. 반환값이 없는 `{CALL ...}` 형태에서는 첫 인자가 1번이다.

`SQLLEN Indicator`는 NULL과 실제 데이터 길이를 구분한다.

- 일반 숫자 입력은 `0`
- 일반 문자열 입력은 `SQL_NTS`
- NULL 입력은 `SQL_NULL_DATA`
- 출력 후 `SQL_NULL_DATA`이면 DB가 NULL을 반환한 것

## 5. 비동기 처리에서 주의할 점

- 현재 DB 매니저는 연결 1개와 워커 1개를 사용한다. 여러 요청을 동시에 넣을 수 있지만 SQL 실행은 큐 순서대로 하나씩 처리된다.
- 결과 콜백도 DB 워커 스레드에서 실행된다. 콜백에서 오래 걸리는 작업이나 DB 워커 종료/재시작을 직접 수행하면 안 된다.
- `StopWorker`는 이미 큐에 들어간 작업을 모두 끝낸 뒤 종료한다. `Disconnect`는 워커 종료 후 ODBC 연결을 끊는다.
- 처리량 때문에 병렬 SQL 실행이 필요해질 때는 프로시저 코드를 바꾸는 것이 아니라 DB 연결 풀과 복수 워커를 추가하는 방향으로 확장해야 한다.

## 6. 현재 로그인 DB의 전제

저장소 안에는 SQL Server 테이블 스키마나 `dbo.tp_UserLogin` 생성 SQL이 없다. 따라서 테이블/프로시저 정의를 추측해서 만들지는 않았다. 실제 DB의 프로시저 파라미터 순서와 타입이 2절의 계약과 다르면 `AuthProcedures.cpp`의 호출 문자열과 바인딩을 실제 정의에 맞춰야 한다.

연결 문자열은 현재 `Server.cpp`에 직접 작성되어 있다. 운영 환경에서는 비밀번호를 소스에 두지 말고 환경 변수나 별도 비공개 설정에서 읽는 방식으로 옮기는 것이 안전하다.
