using System;

namespace LinuxSCP
{
    internal interface ISessionProcess : IDisposable
    {
        event OutputDataReceivedEventHandler OutputDataReceived;
        bool HasExited { get; }
        int ExitCode { get; }

        void Start();
        void ExecuteCommand(string command);

        void Close();
    }
}
