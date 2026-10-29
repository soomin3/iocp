#pragma once

namespace LifetimeVerification
{
	enum class TestGroup { Common, Game };

	inline constexpr const wchar_t* GroupName(TestGroup Group)
	{
		return Group == TestGroup::Common ? L"서버 공통" : L"게임 서버";
	}

	inline constexpr const wchar_t* GroupId(TestGroup Group)
	{
		return Group == TestGroup::Common ? L"common" : L"game";
	}

	struct Scenario
	{
		const wchar_t* id;
		const wchar_t* name;
		const wchar_t* description;
		TestGroup group;
	};

	inline constexpr Scenario kScenarios[] =
	{
		{ L"session_accept_cancel", L"연결 · 수락 중 종료", L"AcceptEx 취소와 성공 완료 후 종료 시 프레임 회수 순서", TestGroup::Common },
		{ L"session_queued_close", L"연결 · 수신 완료와 종료", L"완료를 꺼낸 뒤 종료해도 READ 버퍼와 재개 호출 수명 유지", TestGroup::Common },
		{ L"session_send_close", L"연결 · 송신 중 종료", L"SEND_WAKE 및 WSASend 완료 회수 전 송신 버퍼 보존", TestGroup::Common },
		{ L"session_resume_close", L"세션 · 처리 중 종료", L"패킷 처리기가 실행 중일 때 종료·재사용 차단", TestGroup::Common },
		{ L"session_reuse", L"세션 · 이전 연결의 늦은 종료", L"재사용된 세션에 이전 generation 종료 요청이 영향을 주지 않음", TestGroup::Common },
		{ L"session_parallel_close", L"IOCP · 다중 워커 경합", L"4개 IOCP 워커와 송신·연결 종료를 겹쳐 완료를 모두 회수", TestGroup::Common },
		{ L"session_manager_shutdown", L"IOCP · 서버 전체 종료", L"실제 IOCPManager의 수락 취소·진행 중 처리·워커 종료 순서 (8694 포트)", TestGroup::Common },
		{ L"db_cancel_connect", L"DB 통신 · 연결 수립 중 취소", L"실제 AcceptEx·ConnectEx의 취소 완료 후 프레임 회수", TestGroup::Common },
		{ L"db_cancel_io", L"DB 통신 · 송수신 중 종료", L"대기 중인 송수신의 버퍼를 취소 완료까지 보존", TestGroup::Common },
		{ L"db_late_completion", L"DB 통신 · 완료와 취소 경합", L"이미 도착한 완료와 취소·시간 초과의 처리 순서 확인", TestGroup::Common },
		{ L"db_backend_shutdown", L"DBServer · 지연된 작업과 종료", L"실제 DBClient/DBServer와 모의 DB 작업으로 늦은 응답·종료 검증 (8695 포트)", TestGroup::Common },
		{ L"game_login", L"로그인 · 인증과 중복 접속", L"인증 성공·실패, 같은 계정 중복 로그인, 종료 후 재로그인", TestGroup::Game },
		{ L"game_nickname", L"닉네임 · 생성과 유일성", L"로그인 상태·입력 검증, 생성·중복·이미 생성된 이름의 변경 거부", TestGroup::Game },
		{ L"game_chat", L"채팅 · 권한과 브로드캐스트", L"로그인·닉네임 조건, 여러 사용자의 수신 내용과 발신자 정보", TestGroup::Game },
		{ L"game_chat_limit", L"채팅 · 전송 횟수 제한", L"초당 5회 이후 오류 응답, 제한된 메시지의 미전달", TestGroup::Game },
		{ L"game_stale_response", L"인증 · 종료 후 지연 DB 응답", L"이전 닉네임 DB 응답이 같은 세션으로 재접속한 사용자를 변경하지 않음", TestGroup::Game },
		{ L"game_protocol", L"패킷 · 게임 명령 경계", L"분할·병합된 정상 패킷 처리, 내부 패킷 위조·잘못된 크기 거부", TestGroup::Game },
	};
}
