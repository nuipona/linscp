using System;

namespace LinuxSCP
{
    internal class StdOutException : Exception
    {
        public StdOutException() :
            // when the data are expected, the exception does not leave the API
            base("Unexpected data")
        {
        }
    }
}
