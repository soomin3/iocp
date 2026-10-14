#include "LoadClient.h"
#include <iostream>
#include <stdexcept>

int main(int _Argc, char** _Argv)
{
	try
	{
		SConfig Config = ParseConfig(_Argc, _Argv);
		if (Config.Help)
		{
			PrintUsage();
			return 0;
		}
		// Keep the session population and histograms off the small Windows main-thread stack.
		auto Client = std::make_unique<CLoadClient>(std::move(Config));
		return Client->Run();
	}
	catch (const std::exception& Error)
	{
		std::cerr << "DummyClient: " << Error.what() << '\n';
		return 2;
	}
}
