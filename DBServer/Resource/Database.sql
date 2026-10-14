SET ANSI_NULLS ON;
SET QUOTED_IDENTIFIER ON;
SET ANSI_PADDING ON;
SET ANSI_WARNINGS ON;
SET CONCAT_NULL_YIELDS_NULL ON;
SET ARITHABORT ON;
SET NUMERIC_ROUNDABORT OFF;
SET NOCOUNT ON;
GO

IF COL_LENGTH('dbo.TUser', 'Nickname') IS NULL
BEGIN
    ALTER TABLE dbo.TUser
    ADD Nickname nvarchar(16) NULL;
END
GO

IF NOT EXISTS (
    SELECT 1
    FROM sys.indexes
    WHERE name = 'UX_TUser_Nickname'
      AND object_id = OBJECT_ID('dbo.TUser'))
BEGIN
    CREATE UNIQUE INDEX UX_TUser_Nickname
    ON dbo.TUser(Nickname)
    WHERE Nickname IS NOT NULL;
END
GO

CREATE OR ALTER PROCEDURE dbo.tp_UserLogin
    @ID         nvarchar(64),
    @PW         nvarchar(64),
    @AccountID  bigint OUTPUT,
    @Nickname   nvarchar(16) OUTPUT
AS
BEGIN
    SET NOCOUNT ON;

    SET @AccountID = 0;
    SET @Nickname = NULL;

    SELECT
        @AccountID = idx,
        @Nickname = Nickname
    FROM dbo.TUser
    WHERE ID = @ID
      AND PW = @PW;

    IF @@ROWCOUNT = 0
        RETURN 1;

    RETURN 0;
END
GO

CREATE OR ALTER PROCEDURE dbo.tp_UserCreateNickname
    @AccountID bigint,
    @Nickname  nvarchar(16)
AS
BEGIN
    SET NOCOUNT ON;

    IF @Nickname IS NULL
       OR LEN(LTRIM(RTRIM(@Nickname))) = 0
       OR LEN(@Nickname) > 16
        RETURN 4;

    BEGIN TRY
        UPDATE dbo.TUser
        SET Nickname = @Nickname
        WHERE idx = @AccountID
          AND Nickname IS NULL;

        IF @@ROWCOUNT = 0
        BEGIN
            IF NOT EXISTS (
                SELECT 1
                FROM dbo.TUser
                WHERE idx = @AccountID)
                RETURN 2;

            RETURN 3;
        END

        RETURN 0;
    END TRY
    BEGIN CATCH
        IF ERROR_NUMBER() IN (2601, 2627)
            RETURN 1;

        THROW;
    END CATCH
END
GO
